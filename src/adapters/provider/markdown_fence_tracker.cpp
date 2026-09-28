#include "markdown_fence_tracker.hpp"

#include <cctype>

namespace acecode {

void MarkdownFenceTracker::feed(char c) {
    if (c == '\n') {
        finish_line();
        return;
    }

    // 行内代码跟踪(围栏块内部不跟踪:那里的反引号是代码内容)。
    if (!in_fence_) {
        if (c == '`') {
            if (inline_run_ == 0) {
                // 这串反引号从行首开始 → 可能是围栏开启行而不是行内代码。
                inline_run_is_line_fence_ = line_prefix_active_;
            }
            ++inline_run_;
        } else if (inline_run_ > 0) {
            finish_inline_run();
        }
    }

    if (line_fence_run_active_) {
        if (c == line_fence_char_) {
            ++line_fence_run_;
            if (!in_fence_ && line_fence_run_ >= 3) {
                line_opening_fence_ = true;
            }
            return;
        }

        line_fence_run_active_ = false;
        if (!std::isspace(static_cast<unsigned char>(c))) {
            line_nonspace_after_fence_ = true;
        }
        return;
    }

    if (!line_prefix_active_) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            line_nonspace_after_fence_ = true;
        }
        return;
    }

    if (c == ' ' && line_leading_spaces_ < 3) {
        ++line_leading_spaces_;
        return;
    }

    if (c == '`' || c == '~') {
        line_prefix_active_ = false;
        line_fence_run_active_ = true;
        line_fence_char_ = c;
        line_fence_run_ = 1;
        return;
    }

    line_prefix_active_ = false;
    if (!std::isspace(static_cast<unsigned char>(c))) {
        line_nonspace_after_fence_ = true;
    }
}

void MarkdownFenceTracker::reset() {
    in_fence_ = false;
    fence_char_ = '\0';
    fence_length_ = 0;
    reset_line();
}

bool MarkdownFenceTracker::in_inline_code() const {
    if (in_fence_) return false;
    if (inline_run_ == 0) return inline_open_length_ > 0;
    // 行首 >=3 个反引号是围栏开启行,不是行内代码定界串。
    if (inline_run_is_line_fence_ && inline_run_ >= 3) {
        return inline_open_length_ > 0;
    }
    // 反引号串此刻结束:未打开 → 它打开行内代码;已打开 → 等长才闭合。
    if (inline_open_length_ == 0) return true;
    return inline_run_ != inline_open_length_;
}

void MarkdownFenceTracker::finish_inline_run() {
    if (!(inline_run_is_line_fence_ && inline_run_ >= 3)) {
        if (inline_open_length_ == 0) {
            inline_open_length_ = inline_run_;
        } else if (inline_run_ == inline_open_length_) {
            inline_open_length_ = 0;
        }
    }
    inline_run_ = 0;
    inline_run_is_line_fence_ = false;
}

void MarkdownFenceTracker::finish_line() {
    if (!in_fence_) {
        if (line_opening_fence_) {
            in_fence_ = true;
            fence_char_ = line_fence_char_;
            fence_length_ = line_fence_run_;
        }
    } else if (line_fence_char_ == fence_char_ &&
               line_fence_run_ >= fence_length_ &&
               !line_nonspace_after_fence_) {
        in_fence_ = false;
        fence_char_ = '\0';
        fence_length_ = 0;
    }
    reset_line();
}

void MarkdownFenceTracker::reset_line() {
    line_leading_spaces_ = 0;
    line_prefix_active_ = true;
    line_fence_run_active_ = false;
    line_fence_char_ = '\0';
    line_fence_run_ = 0;
    line_opening_fence_ = false;
    line_nonspace_after_fence_ = false;
    // 未闭合的行内代码不跨行。
    inline_run_ = 0;
    inline_run_is_line_fence_ = false;
    inline_open_length_ = 0;
}

} // namespace acecode
