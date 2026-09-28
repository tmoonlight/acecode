#pragma once

#include "llm/llm_provider.hpp"
#include "markdown_fence_tracker.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace acecode {

struct DsmlToolCallRecoveryResult {
    bool recovered = false;
    std::string visible_text;
    std::vector<ToolCall> tool_calls;
    std::string error;
};

// DeepSeek V4 can emit its native DSML tool protocol through delta.content when
// an OpenAI-compatible gateway fails to translate it to delta.tool_calls. This
// filter keeps normal text streaming while holding a possible DSML marker until
// the complete response can be validated. Raw DSML / special-token markup is
// never returned in visible_text, even when recovery fails.
class DsmlToolCallStreamFilter {
public:
    explicit DsmlToolCallStreamFilter(const std::vector<ToolDef>& tools);

    std::string push(std::string_view chunk);
    DsmlToolCallRecoveryResult finish();
    void reset();

private:
    explicit DsmlToolCallStreamFilter(
        std::unordered_set<std::string> allowed_tools);

    void append_visible_byte(char c, std::string& output);
    bool marker_can_start_here() const;

    std::unordered_set<std::string> allowed_tools_;
    std::string id_scope_;
    std::string marker_probe_;
    std::string candidate_;
    bool capturing_ = false;

    MarkdownFenceTracker fence_;
};

DsmlToolCallRecoveryResult recover_dsml_tool_calls(
    std::string_view text,
    const std::vector<ToolDef>& tools);

} // namespace acecode
