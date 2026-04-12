#pragma once

#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>

namespace acecode::markdown {

// Highlight a code block with language-aware syntax coloring.
// Returns a vector of Elements, one per line, with colored text segments.
std::vector<ftxui::Element> highlight_code(const std::string& code,
                                           const std::string& language);

// Check if a language is supported for highlighting
bool supports_language(const std::string& language);

// Normalize language aliases (e.g., "js" -> "javascript", "py" -> "python")
std::string normalize_language(const std::string& lang);

} // namespace acecode::markdown
