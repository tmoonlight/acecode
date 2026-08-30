#pragma once

#include "markdown_types.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace acecode::markdown {

// Lex raw Markdown text into a flat list of block-level tokens.
// Each block token may contain inline children (populated by parse_inline).
std::vector<Token> lex(const std::string& content);

// Parse inline Markdown within a text span.
// Used internally by lex() for paragraph/heading/list_item text content.
std::vector<Token> parse_inline(const std::string& text);

// True when a completed line holds no inline delimiter that could extend
// into the next line (R7/R8: stack-based matching plus a trailing-backslash
// line-end guard). Shared by the incremental LexerState and the
// StreamingFormatter to decide where the stable prefix may advance.
bool line_is_safe_to_freeze(const std::string& line);

// True when a line opens a fenced code block: optional leading whitespace,
// then at least three identical backticks or tildes. An info string is
// allowed on the opening fence (e.g. ```cpp); for backtick fences the info
// string must not contain a backtick. On success fills fence_char with the
// fence character and fence_count with the fence run length, which the
// caller must retain to match a closing fence via line_closes_code_fence.
// Shared by LexerState and StreamingFormatter.
bool is_code_fence_line(const std::string& line, char& fence_char,
                        int& fence_count);

// True when `line` closes a fenced code block opened by a fence with
// `open_char`/`open_count`. Mirrors the real lexer's closing rule: the line
// starts with the same fence character as the opener, the run is at least as
// long as the opener's (and at least three), and the whole (trimmed) line
// consists only of fence characters (no info string). Any other fence-shaped
// line is code content and leaves the fence open. Shared by LexerState and
// StreamingFormatter.
bool line_closes_code_fence(const std::string& line, char open_char,
                            int open_count);

// Resumable lexer for incremental rendering. append() ingests a delta; a
// completed line (ending in '\n') that is outside any open code fence and
// is safe to freeze advances the stable boundary, and the frozen text is
// lexed into stable_. Everything still unstable stays in pending_, which
// tail_tokens() re-lexes on demand. The invariant at block boundaries is
// stable_tokens() + lex(pending) == lex(full).
class LexerState {
public:
    // Ingest new text; may freeze additional lines into stable_.
    void append(const std::string& delta);
    // Clear pending_ and stable_ (new conversation turn).
    void reset();
    // Tokens frozen so far; append() never rewrites them.
    const std::vector<Token>& stable_tokens() const;
    // Tokens for the still-unstable tail (lex(pending_)).
    std::vector<Token> tail_tokens() const;
    // Number of stable tokens added by the most recent append() call.
    std::size_t new_stable_count() const;

private:
    std::string pending_;
    std::vector<Token> stable_;
    std::size_t last_stable_size_ = 0;
};

} // namespace acecode::markdown
