#include "image_generation_client.hpp"

#include "../../network/proxy_resolver.hpp"
#include "../../utils/logger.hpp"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace acecode::image_generation {

namespace {

// 上游用哪些字眼表示额度/限流。这类失败要和网络失败分开报,否则用户会去
// 排查代理而不是去看余额。
bool looks_like_quota_error(int status_code, const std::string& message) {
    if (status_code == 429) return true;
    if (status_code == 402) return true;
    std::string lowered;
    lowered.reserve(message.size());
    for (unsigned char c : message) {
        lowered.push_back(static_cast<char>(std::tolower(c)));
    }
    return lowered.find("quota") != std::string::npos ||
           lowered.find("rate limit") != std::string::npos ||
           lowered.find("rate_limit") != std::string::npos ||
           lowered.find("insufficient") != std::string::npos ||
           lowered.find("billing") != std::string::npos;
}

// 上游的错误对象形状不统一:有的是 {"error":{"message":...}},有的是
// {"error":"..."},有的干脆只有 message。逐个试。
std::string extract_error_message(const nlohmann::json& body) {
    if (!body.is_object()) return {};
    if (body.contains("error")) {
        const auto& err = body["error"];
        if (err.is_string()) return err.get<std::string>();
        if (err.is_object()) {
            if (err.contains("message") && err["message"].is_string()) {
                return err["message"].get<std::string>();
            }
            return err.dump();
        }
    }
    if (body.contains("message") && body["message"].is_string()) {
        return body["message"].get<std::string>();
    }
    return {};
}

} // namespace

ImageResponse parse_image_response(int status_code,
                                   const std::string& body,
                                   const std::string& transport_error) {
    ImageResponse out;

    if (status_code == 0) {
        out.error = transport_error.empty()
                        ? "image request failed before receiving a response"
                        : transport_error;
        return out;
    }

    nlohmann::json parsed;
    bool parsed_ok = false;
    try {
        parsed = nlohmann::json::parse(body);
        parsed_ok = true;
    } catch (const std::exception&) {
        parsed_ok = false;
    }

    if (status_code < 200 || status_code >= 300) {
        std::string message = parsed_ok ? extract_error_message(parsed) : std::string{};
        if (message.empty()) {
            message = "upstream returned HTTP " + std::to_string(status_code);
        }
        out.error = message;
        out.quota_error = looks_like_quota_error(status_code, message);
        return out;
    }

    if (!parsed_ok) {
        out.error = "upstream returned a non-JSON body";
        return out;
    }

    // 2xx 也可能带 error 对象(部分网关这么干)。
    const std::string embedded_error = extract_error_message(parsed);
    if (!embedded_error.empty()) {
        out.error = embedded_error;
        out.quota_error = looks_like_quota_error(status_code, embedded_error);
        return out;
    }

    if (!parsed.contains("data") || !parsed["data"].is_array() ||
        parsed["data"].empty()) {
        out.error = "upstream returned no image data";
        return out;
    }

    const auto& first = parsed["data"][0];
    if (!first.is_object()) {
        out.error = "upstream returned no image data";
        return out;
    }

    std::string b64;
    if (first.contains("b64_json") && first["b64_json"].is_string()) {
        b64 = first["b64_json"].get<std::string>();
    } else if (first.contains("url") && first["url"].is_string()) {
        // response_format=url 时上游实测回的是 data URL 而不是远端链接,
        // 所以这条分支也要能吃下来。
        const std::string url = first["url"].get<std::string>();
        const std::string marker = ";base64,";
        const auto pos = url.find(marker);
        if (url.rfind("data:", 0) == 0 && pos != std::string::npos) {
            b64 = url.substr(pos + marker.size());
            const auto mime_end = url.find(';');
            if (mime_end != std::string::npos && mime_end > 5) {
                out.mime_type = url.substr(5, mime_end - 5);
            }
        } else {
            out.error = "upstream returned a remote image URL, which is not supported";
            return out;
        }
    }

    if (b64.empty()) {
        out.error = "upstream returned no image data";
        return out;
    }

    out.b64_data = std::move(b64);
    if (first.contains("revised_prompt") && first["revised_prompt"].is_string()) {
        out.revised_prompt = first["revised_prompt"].get<std::string>();
    }
    if (first.contains("width") && first["width"].is_number_integer()) {
        out.width = first["width"].get<int>();
    }
    if (first.contains("height") && first["height"].is_number_integer()) {
        out.height = first["height"].get<int>();
    }
    out.ok = true;
    return out;
}

namespace {

struct RawHttpResult {
    int status_code = 0;
    std::string body;
    std::string transport_error;
};

RawHttpResult post_generations(const ImageRequest& request) {
    const std::string url = request.base_url + "/images/generations";
    auto proxy_opts = network::proxy_options_for(url);

    nlohmann::json payload = {
        {"model", request.model},
        {"prompt", request.prompt},
    };

    cpr::Response resp = cpr::Post(
        cpr::Url{url},
        cpr::Header{
            {"Authorization", "Bearer " + request.api_key},
            {"Content-Type", "application/json"},
        },
        cpr::Body{payload.dump()},
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{request.timeout_ms});

    RawHttpResult out;
    out.status_code = static_cast<int>(resp.status_code);
    out.body = resp.text;
    if (resp.status_code == 0) out.transport_error = resp.error.message;
    return out;
}

RawHttpResult post_edits(const ImageRequest& request) {
    const std::string url = request.base_url + "/images/edits";
    auto proxy_opts = network::proxy_options_for(url);

    cpr::Multipart form{
        {"model", request.model},
        {"prompt", request.prompt},
    };
    for (const auto& path : request.reference_image_paths) {
        form.parts.emplace_back("image", cpr::File{path});
    }

    cpr::Response resp = cpr::Post(
        cpr::Url{url},
        cpr::Header{{"Authorization", "Bearer " + request.api_key}},
        form,
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{request.timeout_ms});

    RawHttpResult out;
    out.status_code = static_cast<int>(resp.status_code);
    out.body = resp.text;
    if (resp.status_code == 0) out.transport_error = resp.error.message;
    return out;
}

ImageResponse run_blocking(const ImageRequest& request) {
    RawHttpResult raw = request.reference_image_paths.empty()
                            ? post_generations(request)
                            : post_edits(request);
    // 尺寸由上游报(实测 data[0] 带 width/height)。报不报都不影响成败 ——
    // 拿不到就在输出里省略尺寸,不为此多解一遍 base64。
    return parse_image_response(raw.status_code, raw.body, raw.transport_error);
}

} // namespace

ImageResponse execute_image_request(const ImageRequest& request,
                                    const std::atomic<bool>* abort_flag) {
    if (!abort_flag) {
        return run_blocking(request);
    }

    // 单张 20~60 秒,4k 更久。同步执行会让用户的停止在整段调用期间失效
    // (McpManager::invoke 踩过同一个坑),所以阻塞调用放 detached 线程,
    // 本线程 100ms 轮询 abort。迟到的响应写进 box 后整体丢弃。
    struct ResultBox {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        ImageResponse result;
    };
    auto box = std::make_shared<ResultBox>();
    std::thread([request, box]() {
        ImageResponse r = run_blocking(request);
        std::lock_guard<std::mutex> lk(box->mu);
        box->result = std::move(r);
        box->done = true;
        box->cv.notify_all();
    }).detach();

    std::unique_lock<std::mutex> lk(box->mu);
    while (!box->done) {
        box->cv.wait_for(lk, std::chrono::milliseconds(100));
        if (!box->done && abort_flag->load()) {
            LOG_INFO("[image_generate] request abandoned by user abort");
            ImageResponse aborted;
            aborted.aborted = true;
            aborted.error =
                "image generation abandoned because the user aborted the turn";
            return aborted;
        }
    }
    return std::move(box->result);
}

} // namespace acecode::image_generation
