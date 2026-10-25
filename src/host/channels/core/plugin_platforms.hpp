#pragma once

// 外部平台插件清单(openspec add-channel-platform-plugins D1):
// daemon 启动时扫描 <data_dir>/connectors/*/channel-platform.json,合法的清单变成一条
// PlatformSpec(plugin 字段非空),由调用方交给 register_plugin_platforms。
// 不合法的清单整份跳过,原因进 errors(由调用方写日志),不影响其它平台。

#include "channels/core/platforms.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace acecode::channels::core {

inline constexpr const char* kPluginManifestFile = "channel-platform.json";
inline constexpr const char* kPluginManifestSchema = "acecode.channel-platform.v1";

// manifest_dir 为清单所在目录(用于解析相对 command 与替换 ${manifest_dir})。
std::optional<PlatformSpec> parse_plugin_manifest(const nlohmann::json& manifest,
                                                  const std::filesystem::path& manifest_dir, std::string* error);

struct PluginScanResult {
    std::vector<PlatformSpec> platforms;
    std::vector<std::string> errors;  // 每条一行,带清单路径,不含清单以外的内容
};

// connectors_root 不存在时返回空结果。同名插件只保留按目录名排序后的第一个。
PluginScanResult scan_plugin_platforms(const std::filesystem::path& connectors_root);

} // namespace acecode::channels::core
