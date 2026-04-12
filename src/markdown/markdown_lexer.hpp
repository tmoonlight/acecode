#pragma once

#include "markdown_types.hpp"
#include <string>
#include <vector>

namespace acecode::markdown {

// Lex raw Markdown text into a flat list of block-level tokens.
// Each block token may contain inline children (populated by parse_inline).
std::vector<Token> lex(const std::string& content);

// Parse inline Markdown within a text span.
// Used internally by lex() for paragraph/heading/list_item text content.
std::vector<Token> parse_inline(const std::string& text);

} // namespace acecode::markdown
