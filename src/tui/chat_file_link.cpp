#include "chat_file_link.hpp"

#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace acecode::tui {
namespace {

std::string trim_copy(const std::string& value) {
    auto first = std::find_if_not(
        value.begin(),
        value.end(),
        [](unsigned char ch) { return std::isspace(ch) != 0; });
    auto last = std::find_if_not(
        value.rbegin(),
        value.rend(),
        [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

bool looks_like_windows_drive_path(const std::string& value) {
    return value.size() >= 3 &&
           std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
           value[1] == ':' &&
           (value[2] == '\\' || value[2] == '/');
}

std::size_t numeric_suffix_start(
    const std::string& value,
    std::size_t end) {
    std::size_t digits = end;
    while (digits > 0 &&
           std::isdigit(static_cast<unsigned char>(value[digits - 1])) != 0) {
        --digits;
    }
    if (digits == end || digits == 0 || value[digits - 1] != ':') {
        return std::string::npos;
    }
    return digits - 1;
}

std::string strip_source_location_suffix(const std::string& value) {
    const std::size_t last = numeric_suffix_start(value, value.size());
    if (last == std::string::npos) return value;

    const std::size_t previous = numeric_suffix_start(value, last);
    return value.substr(
        0,
        previous == std::string::npos ? last : previous);
}

bool has_url_scheme(const std::string& value) {
    if (value.empty() ||
        std::isalpha(static_cast<unsigned char>(value[0])) == 0) {
        return false;
    }
    for (std::size_t i = 1; i < value.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (ch == ':') return true;
        if (std::isalnum(ch) == 0 && ch != '+' && ch != '-' && ch != '.') {
            return false;
        }
    }
    return false;
}

bool is_supported_existing_target(const fs::path& path, std::string& error) {
    std::error_code ec;
    if (fs::is_regular_file(path, ec) || fs::is_directory(path, ec)) {
        return true;
    }
    if (ec) {
        error = "Failed to inspect local link target";
    }
    return false;
}

TuiChatFileLinkResult resolved_existing_target(const fs::path& path) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(path, ec);
    if (ec || canonical.empty()) {
        return {
            true,
            false,
            {},
            "Failed to resolve local link target",
        };
    }
    return {true, true, std::move(canonical), {}};
}

} // namespace

TuiChatFileLinkResult resolve_tui_chat_file_link(
    const std::string& href,
    const std::string& cwd_utf8) {
    const std::string trimmed = trim_copy(href);
    if (trimmed.empty() || trimmed.front() == '#' ||
        trimmed.rfind("//", 0) == 0) {
        return {};
    }

    const bool windows_drive = looks_like_windows_drive_path(trimmed);
    const std::string path_text = strip_source_location_suffix(trimmed);
    if (path_text.empty() || (!windows_drive && has_url_scheme(path_text))) {
        return {};
    }

    const fs::path requested = acecode::path_from_utf8(path_text);
    if (requested.empty()) {
        return {
            true,
            false,
            {},
            "Local link path is empty",
        };
    }

    if (requested.is_absolute()) {
        std::string inspect_error;
        if (!is_supported_existing_target(requested, inspect_error)) {
            return {
                true,
                false,
                {},
                inspect_error.empty()
                    ? "Local link target does not exist"
                    : std::move(inspect_error),
            };
        }
        return resolved_existing_target(requested);
    }

    std::error_code ec;
    fs::path base = acecode::path_from_utf8(cwd_utf8);
    if (!base.is_absolute()) {
        base = fs::absolute(base, ec);
        if (ec || base.empty()) {
            return {
                true,
                false,
                {},
                "Active TUI working directory is invalid",
            };
        }
    }
    base = fs::weakly_canonical(base, ec);
    if (ec || base.empty()) {
        return {
            true,
            false,
            {},
            "Active TUI working directory is invalid",
        };
    }

    for (;;) {
        const fs::path candidate = base / requested;
        std::string inspect_error;
        if (is_supported_existing_target(candidate, inspect_error)) {
            return resolved_existing_target(candidate);
        }

        const fs::path parent = base.parent_path();
        if (parent.empty() || parent == base) break;
        base = parent;
    }

    return {
        true,
        false,
        {},
        "Local link target does not exist",
    };
}

TuiChatFileLinkResult open_tui_chat_file_link(
    const std::string& href,
    const std::string& cwd_utf8,
    acecode::desktop::OpenInExplorerLauncher launcher) {
    auto resolved = resolve_tui_chat_file_link(href, cwd_utf8);
    if (!resolved.handled || !resolved.ok) return resolved;

    const auto opened = acecode::desktop::open_path_in_file_manager(
        acecode::path_to_utf8(resolved.path),
        std::move(launcher));
    if (!opened.ok) {
        resolved.ok = false;
        resolved.error = opened.error.empty()
            ? "Failed to open local link in file manager"
            : opened.error;
    }
    return resolved;
}

} // namespace acecode::tui
