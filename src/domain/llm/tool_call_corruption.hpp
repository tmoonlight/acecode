#pragma once

// 损坏工具调用判定。纯字符串 / JSON 逻辑,无 IO:TurnRunner 据此整步丢弃重发,
// ToolErrors 据此决定 Unknown tool 报错回不回显名字,发给模型的历史投影据此
// 藏起旧会话里的坏调用 —— 三处同一口径。
//
// 背景(内网 wizard-ai 网关 aicoder-pro = GLM 5.2 FP8,2026-10-14 daemon 日志):
// GLM 把工具调用写进聊天模板(`<tool_call>名字` 加成对的 `<arg_key>` /
// `<arg_value>`),由服务端解析成结构化 tool_calls。高峰期解析失败,返回的函数名
// 变成 `skill_view(name="kimi-webbridge")</arg_value>`、几段调用串在同一个名字
// 里,或者干脆是空名字 + `{}`。这类调用执行了也只会报 Unknown tool,坏名字还会
// 写回历史,下一轮被服务端按模板排回提示词,等于给模型看错误示范。

#include "llm/llm_provider.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acecode {

// 工具名字符集 ^[A-Za-z0-9_-]{1,64}$(与「工具重写」模型侧名同一规则,也是
// OpenAI / Anthropic 对 function name 的约束)。
bool is_conventional_tool_name(std::string_view name);

// 文本里最早出现的 GLM 工具调用模板标记(<tool_call> </tool_call> <arg_key>
// </arg_key> <arg_value> </arg_value>),没有返回空。
std::optional<std::string> find_glm_tool_call_marker(std::string_view text);

struct ToolCallCorruption {
    enum class Field { Name, Arguments };
    enum class Reason { EmptyName, TemplateMarkup, CallSyntax };
    Field field = Field::Name;
    Reason reason = Reason::EmptyName;
    // 命中的模板标记或调用语法字符;空名字时为空。
    std::string marker;
};

// 工具名损坏:名字为空(或只有空白);或不符合工具名字符集,且含 GLM 模板标记
// 或调用语法(圆括号、方括号、花括号、引号、等号、尖括号、反引号)。格式正常但
// 不存在的名字(`Bash`、`functions.bash`)不算,那走 Unknown tool,报错会列出
// 可用工具。已注册的工具名由调用方先排除。
std::optional<ToolCallCorruption> diagnose_corrupted_tool_name(std::string_view name);

// 参数损坏:只认模板残片的结构,不认单个标记。模型要编辑的文件本身可能含这些
// 标记(ACECode 自己的源码和测试就有),子串一出现就拦,这类编辑会永远失败。
// 命中条件:
//   - 参数不是合法 JSON,且含任一 GLM 标记(这份参数本来就没法用);
//   - JSON 对象的键名含任一 GLM 标记(键名是参数名,不会合法地带标签);
//   - 字符串值里出现「`</arg_key>` 后(可隔空白)紧跟 `<arg_value>`」或
//     「`</arg_value>` 后紧跟 `<arg_key>` / `<tool_call>` / `</tool_call>`」
//     (相邻参数的模板漏进了这个值),或值以 `</arg_value>` 结尾(值自己的
//     闭合标签没剥掉)。
std::optional<ToolCallCorruption> diagnose_corrupted_tool_arguments(std::string_view arguments);

struct CorruptedToolCall {
    std::size_t index = 0;  // 在 calls 中的下标
    ToolCallCorruption corruption;
};

// 逐个检查一步回复里的工具调用,返回第一个损坏的。is_registered 判为已注册的
// 名字不按名字判(参数照查);is_registered 为空时只按名字本身判。
std::optional<CorruptedToolCall> find_corrupted_tool_call(
    const std::vector<ToolCall>& calls,
    const std::function<bool(const std::string&)>& is_registered);

} // namespace acecode
