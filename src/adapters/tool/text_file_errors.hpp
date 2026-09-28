#pragma once

#include "utils/text_file_buffer.hpp"

namespace acecode {

// Add model-facing recovery instructions to codec errors. The error kind and
// all file data remain unchanged; decode metadata mirrors the displayed error.
TextBufferResult with_text_file_tool_errors(TextBufferResult result);
TextEncodeResult with_text_file_tool_errors(TextEncodeResult result);

} // namespace acecode
