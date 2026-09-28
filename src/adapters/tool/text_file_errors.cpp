#include "text_file_errors.hpp"

#include "llm/tool_protocol_names.hpp"

namespace acecode {
namespace {

std::string format_text_file_error(TextFileError kind, const std::string& fallback) {
    if (kind == TextFileError::None) return fallback;
    const auto read_tool_name = model_tool_name_for_native("file_read");
    switch (kind) {
        case TextFileError::AmbiguousUtf8Bom:
            return "[Error] UTF-8 BOM file can be read with " + read_tool_name +
                " using lossy decoding, but its bytes are too ambiguous to edit safely.";
        case TextFileError::DamagedUtf8:
            return "[Error] File can be read with " + read_tool_name +
                " using lossy UTF-8 decoding, but its encoding is too ambiguous to edit safely.";
        case TextFileError::UnknownEncoding:
            return "[Error] File can be read with " + read_tool_name +
                " using lossy decoding, but its encoding is too ambiguous to edit safely.";
        case TextFileError::LossyWrite:
            return "[Error] Target file cannot be safely written as text because it was decoded lossily. Use " +
                read_tool_name + " for inspection and convert the file to a confirmed encoding before editing.";
        case TextFileError::None:
            return fallback;
    }
    return fallback;
}

} // namespace

TextBufferResult with_text_file_tool_errors(TextBufferResult result) {
    if (!result.success && result.error_kind != TextFileError::None) {
        result.error = format_text_file_error(result.error_kind, result.error);
        result.buffer.metadata.error = result.error;
    }
    return result;
}

TextEncodeResult with_text_file_tool_errors(TextEncodeResult result) {
    if (!result.success) {
        result.error = format_text_file_error(result.error_kind, result.error);
    }
    return result;
}

} // namespace acecode
