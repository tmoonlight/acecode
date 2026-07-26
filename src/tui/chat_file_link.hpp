#pragma once

#include "desktop/open_in_explorer.hpp"

#include <filesystem>
#include <string>

namespace acecode::tui {

struct TuiChatFileLinkResult {
    // false means this href is not a local filesystem link and the caller
    // should preserve the existing mouse-event path.
    bool handled = false;
    bool ok = false;
    std::filesystem::path path;
    std::string error;
};

TuiChatFileLinkResult resolve_tui_chat_file_link(
    const std::string& href,
    const std::string& cwd_utf8);

TuiChatFileLinkResult open_tui_chat_file_link(
    const std::string& href,
    const std::string& cwd_utf8,
    acecode::desktop::OpenInExplorerLauncher launcher = {});

} // namespace acecode::tui
