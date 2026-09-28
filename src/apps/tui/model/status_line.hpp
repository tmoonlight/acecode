#pragma once
#include "platform/clipboard.hpp"
#include <chrono>
#include <string>
namespace acecode { struct TuiState; }
namespace acecode::tui {
void set_transient_status_line_locked(TuiState& state, const std::string& message,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
std::string clipboard_paste_status_message(ClipboardTextReadResult::Status status);
std::string clipboard_image_status_message(ClipboardImageReadResult::Status status);
std::string clipboard_copy_status_message(ClipboardTextWriteResult::Status status);
}
