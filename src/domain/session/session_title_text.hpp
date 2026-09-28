#pragma once

#include <string>

namespace acecode {

// 标题生成前的输入判定,沿用既有空白规则,避免为空输入调用模型。
bool has_session_title_input(const std::string& text);

// Validate and clamp a user-supplied title in place.
//
// Rejects (returns false, sets error_out): any C0 control byte except that the
// caller has already excluded — specifically rejects ESC (0x1B), BEL (0x07),
// NUL (0x00), CR (0x0D), LF (0x0A), and HT (0x09). Tabs/newlines are rejected
// because OSC 2 is a single-line construct.
//
// On success (returns true): if the byte length exceeds 256, truncates to a
// UTF-8 character boundary <= 256 bytes. error_out is set to a short note when
// truncation occurred ("truncated"), empty otherwise.
bool sanitize_title(std::string& inout, std::string& error_out);

// Provider implementations use a bracketed error marker for non-streaming
// failures. Keep this deliberately narrower than a generic "Error" prefix so
// legitimate titles such as "Error handling cleanup" remain valid.
bool is_generated_session_error_title(const std::string& title);

// Normalize model-generated title output. JSON and a single Markdown-wrapped
// JSON object are accepted for compatibility; explanations, code blocks,
// malformed structured output, and overlong text are rejected.
std::string sanitize_generated_session_title(std::string raw);

} // namespace acecode
