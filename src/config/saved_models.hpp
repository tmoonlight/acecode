// saved_models: 命名模型注册表的 schema + 校验。
// 对应 openspec/changes/model-profiles 的 Section 1 —— ModelProfile 数据模型。
#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace acecode {

struct ModelReasoningOptions {
    bool supported = false;
    bool mandatory = false;
    bool default_enabled = false;
    std::optional<bool> enabled;
    std::vector<std::string> supported_efforts;
    std::optional<std::string> default_effort;
    std::optional<std::string> effort;
    bool supports_max_tokens = false;
    std::optional<int> max_tokens;
};

// 一个命名模型条目。自包含 —— 一个 entry 就够 create_provider_from_entry 构造
// 出一个可用 provider 实例。name 保留前缀 `"("` 给 ACECode 合成的特殊 name
// (例如 `(session:XXXX)`);user-defined name MUST NOT 以 `(` 开头。
struct ModelProfile {
    std::string name;
    std::string provider;  // "openai" | "anthropic" | "copilot" | "grok" | legacy "codex"
    std::string base_url;  // openai/anthropic 必填;managed providers 忽略
    std::string api_key;   // openai/anthropic 必填
    std::string model;     // 模型标识,必填
    std::optional<std::string> models_dev_provider_id;  // 可选,给 context resolver 的 hint
    std::optional<int> context_window;  // 可选,手动覆盖该模型的上下文窗口(token 数)
    std::optional<int> stream_timeout_ms; // 可选,streaming request timeout(ms)
    std::vector<std::string> capabilities; // 用户声明的能力标签,如 vision/tool_use/web_search
    // Missing on legacy profiles. New profiles use base_url or full_url.
    std::optional<std::string> endpoint_mode;
    std::optional<int> max_output_tokens;
    // catalog/manual marks capabilities as authoritative. Missing preserves
    // the legacy capability interpretation.
    std::optional<std::string> capabilities_source;
    std::optional<ModelReasoningOptions> reasoning;
    std::map<std::string, std::string> request_headers; // openai/anthropic 自定义请求头模板
    // 兼容外部登录器写入的 legacy 管理标记。ACECode 仍允许正常编辑/删除;
    // 成功编辑会用 SavedModelDraft 重建条目并自然清除此标记。
    bool readonly = false;
};

// 解析失败时的描述。line_hint = -1 表示无具体行号信息。
struct SavedModelsValidationError {
    std::string message;
    int line_hint = -1;
};

struct SavedModelNameRepair {
    std::size_t index = 0;
    std::string original_name;
    std::string repaired_name;
};

// 纯函数:把 `saved_models` JSON 数组解析为 ModelProfile vector。
// 成功返回 vector(可为空);失败返回 nullopt 并向 err 写入详细原因。
std::optional<std::vector<ModelProfile>> parse_saved_models(const nlohmann::json& node,
                                                           std::string& err);

// 纯函数:把同名 entry 的最后一条视为最新并保留原名;更旧的条目追加
// `-` + 6 位小写十六进制 hash。生成名称会避开输入及本轮已生成的全部名称。
// 失败时 entries 保持不变。
std::optional<std::vector<SavedModelNameRepair>>
repair_duplicate_saved_model_names(std::vector<ModelProfile>& entries,
                                   std::string& err);

// 纯函数:校验 entry 列表 + default_name 的合法性。
// 校验点:
//  - 各 entry 的 name 非空、不以 `(` 开头(保留前缀)
//  - 列表内 name 唯一(大小写敏感比较)
//  - provider == "openai" 或 "anthropic" 时 base_url / api_key 必须非空(api_key 允许为"<空字符串>"
//    用于 local LM Studio 等无认证的场景 —— TODO 若后续严格化可改)
//  - provider == "copilot" / "grok" 或 legacy "codex" 时只要求 model 非空
//    (codex 仅为兼容旧配置解析,不代表当前可选择/可运行)
//  - 若 default_name 非空,MUST 指向列表中某 entry 的 name
bool validate_saved_models(const std::vector<ModelProfile>& entries,
                           const std::string& default_name,
                           std::string& err);

std::optional<ModelReasoningOptions> parse_model_reasoning_options(
    const nlohmann::json& node,
    std::string& err);
nlohmann::json model_reasoning_options_to_json(
    const ModelReasoningOptions& options);

// Shared credential/endpoint helpers used by config validation and the saved
// model editor. They never inspect or emit credential values.
bool model_profile_allows_no_api_key(const ModelProfile& profile);
std::string normalize_model_endpoint_identity(const std::string& value);

} // namespace acecode
