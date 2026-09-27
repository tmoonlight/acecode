#include "model_family.hpp"

#include "llm/llm_provider.hpp"
#include "tool_protocol_names.hpp"

#include <algorithm>
#include <cctype>

namespace acecode {

namespace {

std::string ascii_lower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

bool model_prefers_apply_patch(std::string_view model_id) {
    const std::string id = ascii_lower(model_id);
    if (id.empty()) return false;
    if (contains(id, "codex")) return true;
    return contains(id, "gpt-") && !contains(id, "gpt-4") && !contains(id, "oss");
}

ModelFamily detect_model_family(std::string_view model_id) {
    const std::string id = ascii_lower(model_id);
    if (id.empty()) return ModelFamily::Default;
    if (contains(id, "claude")) return ModelFamily::Anthropic;
    if (contains(id, "gemini-")) return ModelFamily::Gemini;
    if (contains(id, "codex")) return ModelFamily::GptCodex;
    if (contains(id, "gpt-4") || contains(id, "gpt-oss")) return ModelFamily::GptLegacy;
    if (contains(id, "gpt-")) return ModelFamily::Gpt;
    // o1 / o3 / o4 系列:整段或 "o3-mini" 这类带分隔符的形态,避免把
    // "moonshot"、"deepseek-v3" 里的 o3 误判。
    for (const char* prefix : {"o1", "o3", "o4"}) {
        const std::string p(prefix);
        if (id == p || id.rfind(p + "-", 0) == 0 || id.rfind("openai/" + p, 0) == 0) {
            return ModelFamily::GptLegacy;
        }
    }
    return ModelFamily::Default;
}

void filter_tool_definitions_for_model(std::vector<ToolDef>& definitions,
                                       bool prefers_apply_patch) {
    std::vector<std::string> hidden;
    const std::string apply_patch_name = model_tool_name_for_native("apply_patch");
    const bool apply_patch_available = std::any_of(
        definitions.begin(), definitions.end(),
        [&](const ToolDef& definition) { return definition.name == apply_patch_name; });
    // 偏好 apply_patch 但它被 expert 能力策略滤掉了:退回 file_edit / file_write,
    // 否则模型一个编辑工具都没有。system prompt 的 apply_patch_mode 用同一条件
    // (guidance_allows("apply_patch")),两边保持一致。
    if (prefers_apply_patch && apply_patch_available) {
        hidden.push_back(model_tool_name_for_native("file_edit"));
        hidden.push_back(model_tool_name_for_native("file_write"));
    } else {
        hidden.push_back(apply_patch_name);
    }
    definitions.erase(
        std::remove_if(definitions.begin(), definitions.end(),
                       [&](const ToolDef& definition) {
                           return std::find(hidden.begin(), hidden.end(),
                                            definition.name) != hidden.end();
                       }),
        definitions.end());
}

} // namespace acecode
