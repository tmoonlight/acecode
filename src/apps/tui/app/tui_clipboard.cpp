#include "tui/app/tui_clipboard.hpp"
#include "utils/base64.hpp"
#include <cstdio>

namespace acecode::tui {
ClipboardTextReadResult TuiClipboard::read_text() { return read_system_clipboard_text(); }
ClipboardImageReadResult TuiClipboard::read_image() { return read_system_clipboard_image(); }
ClipboardTextWriteResult TuiClipboard::write_text(const std::string& text) {
    return write_system_clipboard_text(text);
}
void TuiClipboard::write_osc52(const std::string& text) {
    std::string seq = "\x1b]52;c;" + base64_encode(text) + "\x1b\\";
    std::fwrite(seq.data(), 1, seq.size(), stdout);
    std::fflush(stdout);
}

}
