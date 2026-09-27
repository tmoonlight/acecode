// 覆盖 src/tool/model_family.{hpp,cpp}(openspec add-gpt-apply-patch-adaptation):
//   1. model_prefers_apply_patch —— opencode registry.ts 的 usePatch 规则 + codex 扩展
//   2. detect_model_family —— 模型族判定(GPT / Codex / GPT 旧系 / Anthropic / Gemini / 默认)
//   3. filter_tool_definitions_for_model —— 按偏好裁模型侧工具表,含「工具重写」映射
//      生效与 apply_patch 被策略滤掉时的回退

#include <gtest/gtest.h>

#include "provider/llm_provider.hpp"
#include "tool/model_family.hpp"
#include "tool/tool_protocol_names.hpp"

#include <string>
#include <vector>

namespace {

std::vector<acecode::ToolDef> make_defs(std::initializer_list<const char*> names) {
    std::vector<acecode::ToolDef> defs;
    for (const char* name : names) {
        acecode::ToolDef def;
        def.name = name;
        defs.push_back(def);
    }
    return defs;
}

std::vector<std::string> names_of(const std::vector<acecode::ToolDef>& defs) {
    std::vector<std::string> out;
    for (const auto& def : defs) out.push_back(def.name);
    return out;
}

} // namespace

// 场景:GPT-5 家族与 codex 家族的各种 id 形态(带 provider 前缀、大写、日期后缀)。
// 期望:全部偏好 apply_patch。大小写不敏感,因为 PA 网关 / 自定义 saved_models
// 里的 model 字段大小写不可控。
TEST(ModelFamily, Gpt5AndCodexModelsPreferApplyPatch) {
    for (const char* id : {"gpt-5", "gpt-5-mini", "gpt-5.1", "gpt-5-codex",
                           "openai/gpt-5", "GPT-5-Codex", "gpt-5-2025-08-07",
                           "codex-mini-latest", "gpt-6"}) {
        EXPECT_TRUE(acecode::model_prefers_apply_patch(id)) << id;
    }
}

// 场景:不该切换的 id —— gpt-4 系(4o / 4.1)、开源 gpt-oss、o3、Claude、Gemini、空串。
// 期望:全部保持 file_edit / file_write(偏好为假)。这是 opencode 同款排除项;
// gpt-4 系没有用 apply_patch 训练,切过去反而更差。
TEST(ModelFamily, LegacyAndNonGptModelsKeepFileEdit) {
    for (const char* id : {"gpt-4o", "gpt-4.1", "gpt-4o-mini", "gpt-oss-120b",
                           "o3", "o4-mini", "claude-sonnet-4", "gemini-2.5-pro",
                           "deepseek-v3", "kimi-k2", ""}) {
        EXPECT_FALSE(acecode::model_prefers_apply_patch(id)) << id;
    }
}

// 场景:模型族判定。
// 期望:codex 优先于 gpt(gpt-5-codex 是 GptCodex);gpt-4 / gpt-oss / o 系是 GptLegacy;
// claude → Anthropic;gemini- → Gemini;其余 Default。deepseek-v3 里的 "o3" 不能
// 误判成 GptLegacy(只认整段 / 带分隔符的 o1 / o3 / o4)。
TEST(ModelFamily, DetectsFamilyBySubstring) {
    using acecode::ModelFamily;
    EXPECT_EQ(acecode::detect_model_family("gpt-5"), ModelFamily::Gpt);
    EXPECT_EQ(acecode::detect_model_family("gpt-5-codex"), ModelFamily::GptCodex);
    EXPECT_EQ(acecode::detect_model_family("codex-mini-latest"), ModelFamily::GptCodex);
    EXPECT_EQ(acecode::detect_model_family("gpt-4o"), ModelFamily::GptLegacy);
    EXPECT_EQ(acecode::detect_model_family("gpt-oss-20b"), ModelFamily::GptLegacy);
    EXPECT_EQ(acecode::detect_model_family("o3-mini"), ModelFamily::GptLegacy);
    EXPECT_EQ(acecode::detect_model_family("openai/o4-mini"), ModelFamily::GptLegacy);
    EXPECT_EQ(acecode::detect_model_family("claude-opus-4"), ModelFamily::Anthropic);
    EXPECT_EQ(acecode::detect_model_family("gemini-2.5-flash"), ModelFamily::Gemini);
    EXPECT_EQ(acecode::detect_model_family("deepseek-v3"), ModelFamily::Default);
    EXPECT_EQ(acecode::detect_model_family(""), ModelFamily::Default);
}

// 场景:偏好 apply_patch 的模型,模型侧工具表里同时有 file_edit / file_write / apply_patch。
// 期望:file_edit 与 file_write 被裁掉,apply_patch 与其它工具原序保留。
TEST(ModelFamily, FilterHidesFileEditAndWriteWhenPatchPreferred) {
    acecode::ScopedModelToolNameMappings none({});
    auto defs = make_defs({"bash", "file_read", "file_write", "file_edit", "apply_patch", "grep"});
    acecode::filter_tool_definitions_for_model(defs, true);
    EXPECT_EQ(names_of(defs), (std::vector<std::string>{"bash", "file_read", "apply_patch", "grep"}));
}

// 场景:不偏好 apply_patch 的模型(Claude 等)。
// 期望:只裁掉 apply_patch,file_edit / file_write 保留 —— 非 GPT 模型的工具表
// 与改动前完全一致。
TEST(ModelFamily, FilterHidesApplyPatchOtherwise) {
    acecode::ScopedModelToolNameMappings none({});
    auto defs = make_defs({"bash", "file_read", "file_write", "file_edit", "apply_patch", "grep"});
    acecode::filter_tool_definitions_for_model(defs, false);
    EXPECT_EQ(names_of(defs), (std::vector<std::string>{"bash", "file_read", "file_write", "file_edit", "grep"}));
}

// 场景:「工具重写」生效(file_write → write,file_edit → edit),传入的是模型侧名。
// 期望:按映射后的名字裁,写死原生名会漏裁。
TEST(ModelFamily, FilterUsesModelFacingNamesWhenRewriteActive) {
    acecode::ScopedModelToolNameMappings scoped(
        acecode::default_model_tool_name_mappings());
    auto defs = make_defs({"bash", "read", "write", "edit", "apply_patch"});
    acecode::filter_tool_definitions_for_model(defs, true);
    EXPECT_EQ(names_of(defs), (std::vector<std::string>{"bash", "read", "apply_patch"}));
}

// 场景:偏好 apply_patch,但 expert 能力策略已经把 apply_patch 从定义表里滤掉。
// 期望:回退保留 file_edit / file_write —— 否则模型一个编辑工具都没有。
// 回归:第一版无条件裁掉 edit/write,策略与模型偏好叠加时模型零编辑能力。
TEST(ModelFamily, FilterFallsBackToFileEditWhenApplyPatchUnavailable) {
    acecode::ScopedModelToolNameMappings none({});
    auto defs = make_defs({"bash", "file_read", "file_write", "file_edit"});
    acecode::filter_tool_definitions_for_model(defs, true);
    EXPECT_EQ(names_of(defs), (std::vector<std::string>{"bash", "file_read", "file_write", "file_edit"}));
}
