#pragma once

#include "session_storage.hpp"

#include <string>
#include <vector>

namespace acecode::session_export {

// Build a portable Markdown snapshot of the visible session transcript.
std::string build_markdown(const SessionMeta& meta,
                           const std::vector<ChatMessage>& messages);

// Return a filesystem-safe filename stem while preserving UTF-8 characters.
std::string sanitize_filename_stem(const std::string& preferred,
                                  const std::string& fallback);

// Build the safe UTF-8 filename prefilled in the native Save As dialog.
std::string suggested_markdown_filename(const std::string& preferred,
                                        const std::string& fallback);

} // namespace acecode::session_export
