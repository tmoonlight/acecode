#pragma once

#include "utils/stream_processing.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace acecode {

// A diagnostic copy, independent of the parser's input. Keep the beginning and
// latest bytes even when a healthy, long-lived SSE stream keeps receiving data.
class StreamDiagnosticCapture {
public:
    static constexpr std::size_t kMaxBytes = 64 * 1024;
    static constexpr std::string_view kTruncationMarker =
        "\n[stream diagnostic body truncated]\n";

    void append(std::string_view bytes) {
        if (bytes.empty()) return;
        const auto prefix_bytes = (std::min)(
            bytes.size(), kPrefixBytes - prefix_.size());
        prefix_.append(bytes.data(), prefix_bytes);
        bytes.remove_prefix(prefix_bytes);
        if (bytes.empty()) return;

        if (bytes.size() > kTailBytes - tail_.size()) truncated_ = true;
        if (bytes.size() >= kTailBytes) {
            tail_.assign(bytes.data() + bytes.size() - kTailBytes, kTailBytes);
        } else {
            if (tail_.size() > kTailBytes - bytes.size()) {
                tail_.erase(0, tail_.size() - (kTailBytes - bytes.size()));
            }
            tail_.append(bytes.data(), bytes.size());
        }
    }

    void append(std::string_view bytes, long http_status) {
        // HTTP error bodies participate in quota/retry classification. Keep
        // their existing full-body semantics; only a successful stream's raw
        // diagnostic duplicate is bounded here, not the protocol parser.
        if (http_status < 200 || http_status >= 300) {
            if (!bytes.empty()) http_error_body_.append(bytes.data(), bytes.size());
        } else {
            append(bytes);
        }
    }

    std::string str() const {
        if (!http_error_body_.empty()) return http_error_body_;
        if (!truncated_) return prefix_ + tail_;
        // Inserting a marker must not split a valid UTF-8 codepoint and turn
        // a transport error into a later JSON serialization failure.
        std::string result = prefix_.substr(0, utf8_safe_boundary(prefix_));
        result.append(kTruncationMarker);
        std::size_t start = 0;
        while (start < tail_.size() &&
               (static_cast<unsigned char>(tail_[start]) & 0xC0) == 0x80) ++start;
        result.append(tail_, start, std::string::npos);
        return result;
    }

    std::size_t retained_bytes() const {
        return prefix_.size() + tail_.size() + http_error_body_.size();
    }
    bool truncated() const { return truncated_; }

private:
    static constexpr std::size_t kPrefixBytes =
        (kMaxBytes - kTruncationMarker.size()) / 2;
    static constexpr std::size_t kTailBytes =
        kMaxBytes - kTruncationMarker.size() - kPrefixBytes;
    std::string prefix_;
    std::string tail_;
    std::string http_error_body_;
    bool truncated_ = false;
};

} // namespace acecode
