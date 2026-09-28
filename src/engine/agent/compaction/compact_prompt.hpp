#pragma once

#include "llm/message_predicates.hpp"

#include <string>

namespace acecode {

// These strings intentionally match the pinned Codex checkpoint contract,
// except for the trailing "Output requirements" paragraph of the prompt: the
// compaction request carries no tools, and some models otherwise keep "doing
// the next step" by writing tool calls as text into the summary.
const std::string& get_compact_prompt();
// Appended to the prompt when the previous summarization reply was rejected
// (tool calls, tool-call markup, or blank). ACECode-specific, not from Codex.
const std::string& get_compact_invalid_summary_reminder();
// get_compact_summary_prefix() 随 is_compact_summary_message 下沉到 llm/message_predicates(P2-02)。

// Prefix the model-produced suffix exactly as Codex stores it.
std::string get_compact_user_summary_message(const std::string& summary_text);

} // namespace acecode
