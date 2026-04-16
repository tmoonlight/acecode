#pragma once

#include <string>

namespace acecode {

// Get the structured compact prompt (no-tools preamble + 9-section template)
// is_auto: if true, suppresses follow-up questions in output
std::string get_compact_prompt(bool is_auto = false);

// Extract <summary> content from LLM response, strip <analysis> block
std::string format_compact_summary(const std::string& llm_response);

// Wrap the summary text in the final message format for storage
std::string get_compact_user_summary_message(const std::string& summary_text);

} // namespace acecode
