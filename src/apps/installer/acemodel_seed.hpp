#pragma once

// 安装完成页可选填写 ACEModel Key:把 moonlight / starrylight / aurora 三个预设合并进
// 当前用户的 config.json。逻辑与旧 Inno 安装包的 seed_acemodel.ps1 一致:只按 name 或
// model 匹配更新这三项,其它配置与数组原样保留,手动设置过的能力不覆盖。

#include <filesystem>
#include <optional>
#include <string>

namespace acecode::installer {

inline constexpr const char* kAceModelBaseUrl = "https://ge.bigjuan.xyz/aceapi/v1";
inline constexpr const char* kAceModelDefaultModel = "moonlight";

// config_text 为空(文件不存在或为空)时从空对象开始。返回合并后的完整 JSON 文本。
std::optional<std::string> merge_acemodel_profiles(const std::string& config_text,
                                                   const std::string& api_key,
                                                   std::string* error);

bool seed_acemodel_config_file(const std::filesystem::path& config_path,
                               const std::string& api_key,
                               std::string* error);

}  // namespace acecode::installer
