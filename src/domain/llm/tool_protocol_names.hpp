#pragma once

#include <nlohmann/json.hpp>

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acecode {

struct ToolDef;
struct ChatMessage;

// 一条「原生工具名 → 模型侧工具名」的重写。原生名是 ToolExecutor 里注册的
// 内部 id(file_read / file_write / ...),模型侧名是发给 provider 的工具表、
// 历史 tool_calls、system prompt 与工具文案里出现的名字。
struct ToolProtocolNameMapping {
    std::string native_name;
    std::string public_name;
};
using ToolProtocolNameMappings = std::vector<ToolProtocolNameMapping>;

// 内置种子(OpenCode 风格 read / write / edit / todowrite)。它只用于
// 「工具重写」JSON 首次生成时的默认内容,**不是**进程默认生效映射 ——
// 进程启动后的生效映射为空(模型看到原生名),只有用户在设置里勾选
// 「工具重写」后由 tool_rewrites::apply_to_process 发布。
const ToolProtocolNameMappings& default_model_tool_name_mappings();

// 当前生效映射的快照(线程安全拷贝;空 = 不重写)。
ToolProtocolNameMappings model_tool_name_mappings();

// 发布新的生效映射。非法映射拒绝并返回 false,原映射保持不变。
bool set_model_tool_name_mappings(ToolProtocolNameMappings mappings,
                                  std::string* error = nullptr);

// 测试用 RAII:作用域内替换生效映射,析构时恢复之前的映射。
class ScopedModelToolNameMappings {
public:
    explicit ScopedModelToolNameMappings(ToolProtocolNameMappings mappings);
    ScopedModelToolNameMappings(
        std::initializer_list<ToolProtocolNameMapping> mappings);
    ~ScopedModelToolNameMappings();
    ScopedModelToolNameMappings(const ScopedModelToolNameMappings&) = delete;
    ScopedModelToolNameMappings& operator=(const ScopedModelToolNameMappings&) = delete;

private:
    ToolProtocolNameMappings previous_;
};

// provider 可接受的工具名:^[A-Za-z0-9_-]{1,64}$(OpenAI 与 Anthropic 的交集)。
bool is_valid_model_tool_name(std::string_view name);

std::string model_tool_name_for_native(std::string_view native_name);

std::optional<std::string> native_tool_name_for_public_alias(
    std::string_view public_name);

// 同上,但按 ASCII 大小写不敏感查找当前映射的 public 名。命中多条
// (例如映射里同时有 Read 与 read)时返回 nullopt —— 不猜。只给
// ToolExecutor::resolve_model_tool_name_to_native 的大小写容错一步用,
// 让它不必直接读映射表的内部结构。
std::optional<std::string> native_tool_name_for_public_alias_ci(
    std::string_view public_name);

// ASCII 大小写不敏感相等(非 ASCII 字节逐字节精确比较)。
bool ascii_iequals(std::string_view a, std::string_view b);

// 校验一组映射:名字非空且合法、每条确实改名、native / public 各自不重复、
// public 不与任何 native 撞名(否则模型说出来的名字解析会二义)。
bool validate_model_tool_name_mappings(const ToolProtocolNameMappings& mappings,
                                       std::string* error = nullptr);

// 校验当前生效映射。
bool validate_model_tool_name_mappings(std::string* error = nullptr);

// 把文本里**整词**出现的原生工具名替换为模型侧名。边界 = 前后都不是
// [A-Za-z0-9_],所以 `file_read`、"file_read tool"、"file_read." 会被替换,
// 而 file_read_tool / my_file_read 不会。单遍最长匹配,不会链式重写。
// 只用于我们自己生成的、给模型看的文案(工具描述 / 错误提示 / 守卫提示);
// **绝不能**用于工具输出正文 —— 文件内容里的 file_read 是数据不是名字。
std::string rewrite_model_facing_text(std::string text);
std::string rewrite_model_facing_text(std::string text,
                                      const ToolProtocolNameMappings& mappings);

// 把原生定义翻译成模型侧定义:改名 + 描述文本 / 参数 schema 里的 description
// 字段整词重写;参数结构与其余字段逐字节不变。public 名冲突时返回 false 且
// 不改动 model_definitions。
bool translate_tool_definitions_for_model(
    const std::vector<ToolDef>& native_definitions,
    std::vector<ToolDef>& model_definitions,
    std::string* error = nullptr);

void rewrite_tool_calls_for_model(ChatMessage& message);
void rewrite_tool_calls_for_model(std::vector<ChatMessage>& messages);

} // namespace acecode
