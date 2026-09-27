#pragma once

#include "llm/tool_protocol_names.hpp"

#include <string>
#include <vector>

// 「工具重写」:把 ACECode 内置工具在模型面前改名。这是给特殊审计场景用的
// 显式 opt-in 功能,配置**不进 config.json**,单独存 `<data_dir>/tool-rewrites.json`:
//
//   {
//     "version": 1,
//     "enabled": false,
//     "rewrites": { "file_read": "read", "file_write": "write", ... }
//   }
//
// 语义:enabled=false 时进程内生效映射为空,模型看到原生名;enabled=true 时
// rewrites 里的每一条成为生效映射。文件缺失等价于 enabled=false + 内置种子
// (OpenCode 风格四条),这样用户第一次勾选时右列已有可用的默认值。
// TUI / daemon / headless 三个入口在注册工具之前都要调 load_and_apply,
// 注册期的 public 名冲突检查才拿得到正确的映射。
namespace acecode::tool_rewrites {

constexpr const char* kSettingsFileName = "tool-rewrites.json";
constexpr int kSettingsVersion = 1;

struct ToolRewriteSettings {
    bool enabled = false;
    // native → public,已剔除 no-op(空值 / 与原名相同)。
    ToolProtocolNameMappings rewrites;
};

std::string settings_path(const std::string& data_dir);

ToolRewriteSettings default_settings();

// 解析 JSON 文本。结构非法(不是对象 / rewrites 不是 string→string 对象 /
// enabled 不是布尔)返回 false;no-op 条目静默丢弃;未知字段忽略。
bool parse_settings(const std::string& text,
                    ToolRewriteSettings& out,
                    std::string* error = nullptr);

std::string serialize_settings(const ToolRewriteSettings& settings);

// 结构校验(委托 validate_model_tool_name_mappings)。enabled=false 时
// rewrites 同样要合法 —— 用户勾选的一瞬间它们就会生效。
bool validate_settings(const ToolRewriteSettings& settings,
                       std::string* error = nullptr);

// 结合注册表校验:public 名不得等于任何已注册工具的原生名(自身除外),
// 否则模型说出那个名字时 resolve 会先命中真实工具而不是重写目标。
bool validate_settings_against_tools(
    const ToolRewriteSettings& settings,
    const std::vector<std::string>& registered_tool_names,
    std::string* error = nullptr);

// 读文件。缺失 → 默认值且无 error;损坏 → 默认值 + error 文案(调用方决定
// 是否提示)。永远不抛异常。
ToolRewriteSettings load_settings(const std::string& path,
                                  std::string* error = nullptr);

// 原子写(tmp + rename)。写前做结构校验。
bool save_settings(const std::string& path,
                   const ToolRewriteSettings& settings,
                   std::string* error = nullptr);

ToolProtocolNameMappings effective_mappings(const ToolRewriteSettings& settings);

// 把 settings 的生效映射发布到进程(set_model_tool_name_mappings)。
bool apply_to_process(const ToolRewriteSettings& settings,
                      std::string* error = nullptr);

// 启动入口:读 `<data_dir>/tool-rewrites.json` 并发布。文件损坏或映射非法时
// 记日志并保持「不重写」,绝不让启动失败。返回实际采用的设置。
ToolRewriteSettings load_and_apply(const std::string& data_dir);

} // namespace acecode::tool_rewrites
