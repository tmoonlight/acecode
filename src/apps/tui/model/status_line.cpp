#include "status_line.hpp"
#include "tui/tui_state.hpp"

namespace acecode::tui {

void set_transient_status_line_locked(TuiState& state,
                                             const std::string& message,
                                             std::chrono::steady_clock::time_point now) {
    if (state.status_line_clear_at.time_since_epoch().count() == 0) {
        state.status_line_saved = state.status_line;
    }
    state.status_line = message;
    state.status_line_clear_at =
        now + std::chrono::milliseconds(2000);
}

std::string clipboard_paste_status_message(
    acecode::ClipboardTextReadResult::Status status) {
    using Status = acecode::ClipboardTextReadResult::Status;
    switch (status) {
        case Status::Empty:
            return "Clipboard is empty";
        case Status::TooLarge:
            return "Clipboard text too large (max " +
                   std::to_string(acecode::kMaxClipboardTextBytes / (1024 * 1024)) +
                   " MB)";
        case Status::Unavailable:
#ifdef _WIN32
            return "Clipboard paste unavailable";
#elif defined(__APPLE__)
            return "Clipboard paste unavailable (pbpaste failed)";
#else
            return "Clipboard paste unavailable (install wl-clipboard, xclip, or xsel)";
#endif
        case Status::Success:
        default:
            return "";
    }
}

std::string clipboard_image_status_message(
    acecode::ClipboardImageReadResult::Status status) {
    using Status = acecode::ClipboardImageReadResult::Status;
    switch (status) {
        case Status::Empty:
            return "Clipboard has no image";
        case Status::TooLarge:
            return "Clipboard image too large (max " +
                   std::to_string(acecode::kMaxClipboardImageBytes / (1024 * 1024)) +
                   " MB)";
        case Status::Unavailable:
#ifdef _WIN32
            return "Clipboard image paste unavailable";
#elif defined(__APPLE__)
            return "Clipboard image paste unavailable (install pngpaste)";
#else
            return "Clipboard image paste unavailable (install wl-clipboard or xclip)";
#endif
        case Status::Success:
        default:
            return "";
    }
}

std::string clipboard_copy_status_message(
    acecode::ClipboardTextWriteResult::Status status) {
    using Status = acecode::ClipboardTextWriteResult::Status;
    switch (status) {
        case Status::TooLarge:
            return "Clipboard text too large (max " +
                   std::to_string(acecode::kMaxClipboardTextBytes / (1024 * 1024)) +
                   " MB)";
        case Status::Unavailable:
#ifdef _WIN32
            return "Clipboard copy unavailable";
#elif defined(__APPLE__)
            return "Clipboard copy unavailable (pbcopy failed)";
#else
            return "Clipboard copy unavailable (install wl-clipboard, xclip, or xsel)";
#endif
        case Status::Success:
        default:
            return "";
    }
}


} // namespace acecode::tui
