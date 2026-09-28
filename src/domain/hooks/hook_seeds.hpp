#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace acecode {

// 内置 hook 的只读种子定义,供注册表识别与统一安装事务复用。
struct DefaultHookSeed {
    std::string name;
    std::string source_id;
    std::filesystem::path relative_path;
    std::string definition_sha256;
    std::vector<std::string> previous_definition_sha256s;
};

const std::vector<DefaultHookSeed>& default_hook_seeds();

} // namespace acecode
