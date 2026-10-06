#include "http.hpp"

#include "im/redact.hpp"
#include "network/proxy_resolver.hpp"
#include "utils/utf8_path.hpp"
#include "version.hpp"

#include <cpr/cpr.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace acecode::im {
namespace {

// multipart 的文件名在 Windows 上会经 path::string() 转成 ANSI 代码页,遇到代码页
// 表示不了的字符(emoji 等)直接抛异常;这里统一降为 ASCII,原名由调用方另行展示。
std::string ascii_filename(const std::string& name) {
    std::string out;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        out.push_back(u >= 0x20 && u < 0x7F && c != '"' && c != '\\' && c != '/' ? c : '_');
    }
    const auto dot = out.find_last_of('.');
    const auto stem = out.substr(0, dot == std::string::npos ? out.size() : dot);
    if (stem.find_first_not_of('_') == std::string::npos)
        out = "file" + (dot == std::string::npos ? std::string() : out.substr(dot));
    return out;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read upload file");
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

std::string user_agent() { return std::string("ACECode/") + ACECODE_VERSION; }

HttpResponse http_send(const HttpRequest& request) {
    HttpResponse result;
    cpr::Session session;
    session.SetUrl(cpr::Url{request.url});
    cpr::Header header;
    for (const auto& [name, value] : request.headers) header[name] = value;
    if (header.find("User-Agent") == header.end()) header["User-Agent"] = user_agent();
    session.SetHeader(header);
    session.SetTimeout(cpr::Timeout{request.timeout});
    session.SetConnectTimeout(cpr::ConnectTimeout{std::min<std::chrono::milliseconds>(
        request.timeout, std::chrono::seconds(15))});
    if (request.use_proxy) {
        auto options = network::proxy_options_for(request.url);
        session.SetSslOptions(network::build_ssl_options(options));
        session.SetProxies(std::move(options.proxies));
        session.SetProxyAuth(std::move(options.auth));
    } else {
        session.SetProxies(cpr::Proxies{{"http", ""}, {"https", ""}});
        session.SetSslOptions(cpr::Ssl(cpr::ssl::NoRevoke{true}));
    }

    if (request.cancel) {
        session.SetProgressCallback(cpr::ProgressCallback{
            [&cancel = request.cancel](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t,
                                       intptr_t) {
                return !cancel();
            }});
    }

    cpr::Response response;
    try {
        if (!request.download_to.empty()) {
            std::ofstream out(request.download_to, std::ios::binary | std::ios::trunc);
            if (!out) {
                result.error = "Cannot create download file";
                return result;
            }
            std::uint64_t written = 0;
            bool too_large = false;
            const auto limit = request.max_download_bytes;
            response = session.Download(cpr::WriteCallback{[&written, &too_large, &out, limit](std::string_view data,
                                                                                                intptr_t) {
                written += data.size();
                if (limit && written > limit) {
                    too_large = true;
                    return false;  // 中止传输
                }
                out.write(data.data(), static_cast<std::streamsize>(data.size()));
                return static_cast<bool>(out);
            }});
            out.close();
            if (too_large) {
                std::error_code ec;
                std::filesystem::remove(request.download_to, ec);
                result.status = response.status_code;
                result.too_large = true;
                result.error = "Download exceeds limit";
                return result;
            }
        } else if (!request.parts.empty()) {
            cpr::Multipart multipart{};
            std::vector<std::string> buffers;  // Buffer 只保存指针,数据要活到请求结束
            buffers.reserve(request.parts.size());
            for (const auto& part : request.parts) {
                if (part.file.empty()) {
                    multipart.parts.emplace_back(part.name, part.value, part.content_type);
                    continue;
                }
                buffers.push_back(read_file(part.file));
                const auto& data = buffers.back();
                const auto name = ascii_filename(part.filename.empty() ? path_to_utf8(part.file.filename())
                                                                       : part.filename);
                multipart.parts.emplace_back(
                    part.name, cpr::Buffer{data.begin(), data.end(), std::filesystem::path(name)},
                    part.content_type);
            }
            session.SetMultipart(std::move(multipart));
            response = session.Post();
        } else if (request.method == "POST") {
            session.SetBody(cpr::Body{request.body});
            response = session.Post();
        } else if (request.method == "PUT") {
            session.SetBody(cpr::Body{request.body});
            response = session.Put();
        } else if (request.method == "PATCH") {
            session.SetBody(cpr::Body{request.body});
            response = session.Patch();
        } else if (request.method == "DELETE") {
            if (!request.body.empty()) session.SetBody(cpr::Body{request.body});
            response = session.Delete();
        } else {
            response = session.Get();
        }
    } catch (const std::exception& e) {
        result.error = redact_secrets(e.what());
        return result;
    }
    if (request.cancel && request.cancel()) {
        result.cancelled = true;
        result.error = "Request cancelled";
        return result;
    }
    if (response.error.code != cpr::ErrorCode::OK) {
        result.error = redact_secrets(response.error.message.empty() ? "HTTP request failed"
                                                                    : response.error.message);
        result.status = response.status_code;
        return result;
    }
    result.status = response.status_code;
    result.body = std::move(response.text);
    for (const auto& [name, value] : response.header) {
        std::string key = name;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        result.headers[key] = value;
    }
    return result;
}

} // namespace acecode::im
