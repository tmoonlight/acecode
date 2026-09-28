#pragma once

// 文本文件的安全写(P2-05 自 utils/text_file_buffer 拆出):写前 MCP 配置拦截、按原编码回写、
// 写后往返校验与回滚。它依赖 config/mcp_config 与模型侧工具名,所以属于 tool 层;纯编解码留在
// utils/text_file_buffer。

#include "utils/text_file_buffer.hpp"

#include <functional>
#include <string>

namespace acecode {

struct TextSafeWriteResult {
    bool success = false;
    std::string error;
    bool rolled_back = false;
    bool rollback_failed = false;
};

TextSafeWriteResult safe_write_text_file(
    const std::string& path,
    const std::string& lf_text,
    const TextFileMetadata& metadata,
    const std::function<void(const std::string& path)>& before_write = {});

} // namespace acecode
