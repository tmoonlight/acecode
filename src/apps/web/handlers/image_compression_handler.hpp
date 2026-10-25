#pragma once

// 「压缩图片」开关(设置 > 常规)的 REST 纯函数层:GET 快照与 PUT 解析。IO / 锁 /
// 进程级开关的生效在 routes_image_compression.cpp。

#include "config/config.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace acecode::web {

// GET /api/config/image-compression 的响应体:{enabled}
nlohmann::json image_compression_snapshot(const ImageCompressionConfig& cfg);

// 解析 PUT body。语义是 patch:缺省键沿用 current;enabled 出现时必须是布尔。
// 失败时 out 不变、error 是给用户看的文案。
bool parse_image_compression_request(
    const nlohmann::json& body,
    const ImageCompressionConfig& current,
    ImageCompressionConfig& out,
    std::string& error);

} // namespace acecode::web
