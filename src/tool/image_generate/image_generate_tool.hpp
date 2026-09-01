#pragma once

// image_generate 工具(openspec add-image-generation-tool)。
//
// 文生图 + 图生图。是否提供 reference_image_paths 决定走哪个端点,模型不需要
// 知道端点差异。注册由 config.image_generation 门控 —— 端点解析不出来时
// 不注册,而不是注册一个必然失败的工具(那会让模型反复调用反复失败)。

#include "../tool_executor.hpp"
#include "../../config/config.hpp"

namespace acecode {

// 端点可用时返回工具,否则返回 nullopt(调用方跳过注册)。
std::optional<ToolImpl> create_image_generate_tool(const AppConfig& config);

} // namespace acecode
