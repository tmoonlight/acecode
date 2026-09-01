#pragma once

// 图像生成的 HTTP 客户端(openspec add-image-generation-tool)。
//
// 对接 OpenAI 兼容的 Images API 两个端点:
//   POST <base>/images/generations —— JSON,文生图
//   POST <base>/images/edits       —— multipart,图生图(image=@file)
//
// 响应解析与错误映射是纯函数(parse_image_response),不碰网络,单测直接喂
// 字符串。网络那层只负责发请求 + abort 轮询。

#include <atomic>
#include <string>
#include <vector>

namespace acecode::image_generation {

struct ImageRequest {
    std::string base_url;   // 已去掉尾部斜杠
    std::string api_key;
    std::string model;
    std::string prompt;
    // 空 = 文生图(generations);非空 = 图生图(edits),1..5 个本地文件路径。
    std::vector<std::string> reference_image_paths;
    int timeout_ms = 180000;
};

struct ImageResponse {
    bool ok = false;
    bool aborted = false;
    // 额度/限流类失败与网络失败要分开,用户与模型据此判断是等一会儿还是查配置。
    bool quota_error = false;
    std::string error;  // ok=false 时非空

    std::string b64_data;      // ok=true;不含 data URL 前缀
    std::string mime_type = "image/png";
    int width = 0;
    int height = 0;
    std::string revised_prompt;  // 上游改写后的提示词,可能为空
};

// 纯函数:把 HTTP 状态码 + 响应体解析成 ImageResponse。
// transport_error 非空表示连接层失败(status 通常为 0)。
ImageResponse parse_image_response(int status_code,
                                   const std::string& body,
                                   const std::string& transport_error);

// 发起一次请求。abort_flag 非空时,阻塞的 HTTP 调用放 detached 工作线程,
// 本线程 100ms 轮询 —— 否则一次 60 秒的生成期间用户的停止完全失效
// (同 McpManager::invoke 踩过的坑)。
ImageResponse execute_image_request(const ImageRequest& request,
                                    const std::atomic<bool>* abort_flag);

} // namespace acecode::image_generation
