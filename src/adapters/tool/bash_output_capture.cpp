#include "bash_output_capture.hpp"

#include "session/tool_result_storage.hpp"
#include "utils/encoding.hpp"
#include "utils/stream_processing.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

namespace acecode {

namespace {
constexpr std::size_t kHeadBytes = kBashInlineOutputLimitBytes * 2 / 5;
constexpr std::size_t kTailBytes = kBashInlineOutputLimitBytes - kHeadBytes;
}

BashOutputCapture::BashOutputCapture(bool preserve_full_output,
                                   std::string tool_results_dir)
    : preserve_full_output_(preserve_full_output),
      tool_results_dir_(std::move(tool_results_dir)) {}

void BashOutputCapture::fail(const std::string& reason) {
    if (error_.empty()) error_ = reason;
    if (file_.is_open()) file_.close();
}

bool BashOutputCapture::write(std::string_view text) {
    file_.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file_) {
        fail("Failed to write shell output to " + filepath_);
        return false;
    }
    return true;
}

bool BashOutputCapture::start_file() {
    std::error_code ec;
    std::filesystem::path directory;
    if (tool_results_dir_.empty()) {
        directory = std::filesystem::temp_directory_path(ec);
        if (ec) {
            fail("Failed to locate temporary shell output storage: " + ec.message());
            return false;
        }
        directory /= "acecode-tool-results";
    } else {
        directory = path_from_utf8(tool_results_dir_);
    }
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        fail("Failed to create shell output storage: " + ec.message());
        return false;
    }
    const auto path = directory / ("bash-" + generate_uuid() + ".txt");
    filepath_ = path_to_utf8(path);
    file_.open(path, std::ios::binary | std::ios::out);
    if (!file_) {
        fail("Failed to open shell output storage: " + filepath_);
        return false;
    }
    if (!write(output_)) return false;
    // Release the former inline allocation; only a small prefix is needed to
    // format the existing persisted-output message when execution finishes.
    std::string preview = truncate_utf8_prefix(output_, TOOL_RESULT_PREVIEW_BYTES, "");
    output_.swap(preview);
    return true;
}

bool BashOutputCapture::append(std::string_view text) {
    if (failed() || finished_) return false;
    if (text.empty()) return true;
    total_bytes_ += text.size();

    if (file_.is_open()) return write(text);
    if (!truncated_ && text.size() <= kBashInlineOutputLimitBytes - output_.size()) {
        output_.append(text.data(), text.size());
        return true;
    }
    if (preserve_full_output_) {
        if (!start_file()) return false;
        // A single first chunk can exceed the threshold, so the prefix may
        // need to come partly or wholly from this chunk.
        if (output_.size() < TOOL_RESULT_PREVIEW_BYTES) {
            const std::size_t remaining = TOOL_RESULT_PREVIEW_BYTES - output_.size();
            output_.append(text.data(), (std::min)(text.size(), remaining));
            output_.resize(utf8_safe_boundary(output_));
        }
        return write(text);
    }

    if (!truncated_) {
        append_bounded_utf8_tail(tail_, output_, kTailBytes);
        if (output_.size() < kHeadBytes) {
            const std::size_t remaining = kHeadBytes - output_.size();
            output_.append(text.data(), (std::min)(text.size(), remaining));
        }
        output_ = truncate_utf8_prefix(output_, kHeadBytes, "");
        truncated_ = true;
    }
    append_bounded_utf8_tail(tail_, text, kTailBytes);
    return true;
}

std::string BashOutputCapture::finish() {
    finished_ = true;
    if (file_.is_open()) {
        file_.flush();
        if (!file_) fail("Failed to flush shell output to " + filepath_);
        else {
            file_.close();
            if (file_.fail()) fail("Failed to close shell output storage: " + filepath_);
        }
    }
    if (failed()) {
        std::string message = "[Error] " + error_ +
            ". Complete shell output could not be preserved; execution was stopped.";
        if (!filepath_.empty()) message += "\nPartial output file: " + filepath_;
        if (!output_.empty()) {
            message += "\nOutput preview:\n" +
                truncate_utf8_prefix(output_, TOOL_RESULT_PREVIEW_BYTES, "");
        }
        return message;
    }
    if (!filepath_.empty()) {
        return build_large_tool_result_message(PersistedToolResult{
            filepath_, total_bytes_, output_, total_bytes_ > output_.size()});
    }
    if (truncated_) {
        const std::size_t omitted = total_bytes_ - output_.size() - tail_.size();
        std::string message = output_;
        if (!message.empty() && message.back() != '\n') message += '\n';
        message += "[... " + std::to_string(omitted) + " bytes omitted ...]\n";
        message += tail_;
        return message;
    }
    return output_;
}

} // namespace acecode
