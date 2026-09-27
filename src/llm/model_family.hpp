#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace acecode {

struct ToolDef;

// 模型族(openspec add-gpt-apply-patch-adaptation)。只按模型 id 子串判定,
// 不看 provider 名:PA 内网网关 / OpenAI 兼容代理转发 GPT-5 时 provider 名是
// openai,只有 id 可信。判定规则对齐 opencode `session/system.ts::provider()`。
enum class ModelFamily {
    Default,
    Anthropic,   // claude*
    Gpt,         // gpt-5 / gpt-5.x / gpt-6 ... (偏好 apply_patch)
    GptCodex,    // *codex*(gpt-5-codex / codex-mini ...,偏好 apply_patch)
    GptLegacy,   // gpt-4* / o1 / o3 / o4(保持 file_edit)
    Gemini,      // gemini-*
};

ModelFamily detect_model_family(std::string_view model_id);

// opencode `tool/registry.ts` 的 usePatch 规则:含 "gpt-" 且不含 "gpt-4"、
// 不含 "oss";另加 "codex"(codex-mini-latest 等没有 gpt- 前缀但同样是
// apply_patch 训练出来的)。大小写不敏感。
bool model_prefers_apply_patch(std::string_view model_id);

// 按模型过滤**模型侧**工具定义:偏好 apply_patch 的模型看不到
// file_edit / file_write,其它模型看不到 apply_patch。名字按当前生效的
// 「工具重写」映射取,所以传进来的必须是 get_model_tool_definitions* 的输出。
// 三个工具在 ToolExecutor 里始终注册,这里只裁定义表,不影响执行。
void filter_tool_definitions_for_model(std::vector<ToolDef>& definitions,
                                       bool prefers_apply_patch);

} // namespace acecode
