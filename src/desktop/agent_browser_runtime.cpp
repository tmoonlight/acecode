#include "agent_browser_runtime.hpp"

#include "config/config.hpp"
#include "utils/atomic_file.hpp"
#include "utils/url_encoding.hpp"
#include "utils/utf8_path.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <system_error>

namespace acecode::desktop {
namespace {

std::filesystem::path data_root(const std::string& acecode_dir) {
    return path_from_utf8(acecode_dir.empty() ? get_acecode_dir() : acecode_dir);
}

bool ascii_unreserved(unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' ||
        ch == '~';
}

char hex_digit(unsigned int value) {
    return value < 10 ? static_cast<char>('0' + value)
                      : static_cast<char>('A' + value - 10);
}

bool hex_byte_at(const std::string& value, std::size_t offset) {
    if (offset + 2 >= value.size() || value[offset] != '%') return false;
    return std::isxdigit(static_cast<unsigned char>(value[offset + 1])) != 0 &&
        std::isxdigit(static_cast<unsigned char>(value[offset + 2])) != 0;
}

std::string percent_encode_file_value(const std::string& value,
                                      bool preserve_url_delimiters) {
    std::string encoded;
    encoded.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const unsigned char ch = static_cast<unsigned char>(value[index]);
        if (ascii_unreserved(ch) || ch == '/' || ch == ':' ||
            (preserve_url_delimiters &&
             std::string("?#[]@!$&'()*+,;=").find(static_cast<char>(ch)) !=
                 std::string::npos)) {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }
        if (preserve_url_delimiters && ch == '%' &&
            hex_byte_at(value, index)) {
            encoded.append(value, index, 3);
            index += 2;
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(hex_digit((ch >> 4) & 0xF));
        encoded.push_back(hex_digit(ch & 0xF));
    }
    return encoded;
}

bool windows_drive_path(const std::string& value) {
    return value.size() >= 3 &&
        std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
        value[1] == ':' && (value[2] == '/' || value[2] == '\\');
}

bool unc_path(const std::string& value) {
    return value.size() >= 3 &&
        ((value[0] == '\\' && value[1] == '\\') ||
         (value[0] == '/' && value[1] == '/'));
}

std::string local_path_to_file_url(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.rfind("//", 0) == 0) {
        return "file://" + percent_encode_file_value(value.substr(2), false);
    }
    if (windows_drive_path(value)) {
        return "file:///" + percent_encode_file_value(value, false);
    }
    return "file://" + percent_encode_file_value(value, false);
}

std::string normalize_file_url(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    std::string rest = value.substr(5);
    if (rest.rfind("//", 0) == 0) {
        return "file:" + percent_encode_file_value(rest, true);
    }
    if (!rest.empty() && rest.front() == '/') {
        return "file://" + percent_encode_file_value(rest, true);
    }
    if (windows_drive_path(rest)) return local_path_to_file_url(rest);
    return "file:///" + percent_encode_file_value(rest, true);
}

} // namespace

std::filesystem::path agent_browser_root_path(const std::string& acecode_dir) {
    return data_root(acecode_dir) / "agent-browser";
}

std::filesystem::path agent_browser_user_data_path(const std::string& acecode_dir) {
    return agent_browser_root_path(acecode_dir) / "webview2";
}

std::filesystem::path agent_browser_macos_profile_identifier_path(
    const std::string& acecode_dir) {
    return agent_browser_root_path(acecode_dir) / "macos-profile-id";
}

std::filesystem::path agent_browser_proxy_socket_path(
    const std::string& acecode_dir) {
    return data_root(acecode_dir) / "run" / "agent-browser.sock";
}

std::filesystem::path agent_browser_runtime_manifest_path(
    const std::string& acecode_dir) {
    return data_root(acecode_dir) / "run" / "agent-browser.json";
}

std::optional<std::string> normalize_agent_browser_url(
    const std::string& input,
    std::string* error) {
    if (error) error->clear();
    const auto first = std::find_if_not(input.begin(), input.end(), [](char ch) {
        return std::isspace(static_cast<unsigned char>(ch)) != 0;
    });
    const auto last = std::find_if_not(input.rbegin(), input.rend(), [](char ch) {
        return std::isspace(static_cast<unsigned char>(ch)) != 0;
    }).base();
    if (first >= last) {
        if (error) *error = "browser URL is empty";
        return std::nullopt;
    }

    std::string value(first, last);
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    if (lower == "about:blank") return std::string("about:blank");
    if (lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0) {
        return value;
    }
    if (lower.rfind("file:", 0) == 0) return normalize_file_url(value);
    if ((!value.empty() && value.front() == '/') ||
        windows_drive_path(value) || unc_path(value)) {
        return local_path_to_file_url(value);
    }
    if (lower.find("://") != std::string::npos ||
        lower.rfind("javascript:", 0) == 0 ||
        lower.rfind("data:", 0) == 0 ||
        lower.rfind("edge:", 0) == 0 ||
        lower.rfind("devtools:", 0) == 0) {
        if (error) *error = "browser URL scheme is not allowed";
        return std::nullopt;
    }
    if (std::any_of(value.begin(), value.end(), [](char ch) {
            return std::isspace(static_cast<unsigned char>(ch)) != 0;
        })) {
        return "https://www.bing.com/search?q=" +
               acecode::utils::percent_encode_query_component(value);
    }
    return "https://" + value;
}

bool write_agent_browser_runtime_manifest(
    const AgentBrowserRuntimeManifest& manifest,
    const std::string& acecode_dir) {
    const nlohmann::json value{
        {"protocol_version", manifest.protocol_version},
        {"desktop_pid", manifest.desktop_pid},
        {"desktop_instance_id", manifest.desktop_instance_id},
        {"user_data_dir", manifest.user_data_dir},
        {"pipe_name", manifest.pipe_name},
        {"auth_token", manifest.auth_token},
        {"ready_at_ms", manifest.ready_at_ms},
    };
    return atomic_write_file(
        path_to_utf8(agent_browser_runtime_manifest_path(acecode_dir)),
        value.dump(),
        /*restrict_permissions=*/true);
}

std::optional<AgentBrowserRuntimeManifest> read_agent_browser_runtime_manifest(
    const std::string& acecode_dir) {
    std::ifstream input(agent_browser_runtime_manifest_path(acecode_dir),
                        std::ios::binary);
    if (!input.is_open()) return std::nullopt;

    try {
        nlohmann::json value;
        input >> value;
        if (!value.is_object()) return std::nullopt;
        AgentBrowserRuntimeManifest manifest;
        manifest.protocol_version = value.value("protocol_version", 0);
        manifest.desktop_pid = value.value("desktop_pid", std::int64_t{0});
        manifest.desktop_instance_id = value.value("desktop_instance_id", "");
        manifest.user_data_dir = value.value("user_data_dir", "");
        manifest.pipe_name = value.value("pipe_name", "");
        manifest.auth_token = value.value("auth_token", "");
        manifest.ready_at_ms = value.value("ready_at_ms", std::int64_t{0});
        return manifest;
    } catch (...) {
        return std::nullopt;
    }
}

std::string validate_agent_browser_runtime_manifest(
    const AgentBrowserRuntimeManifest& manifest,
    const std::function<bool(std::int64_t)>& pid_alive) {
    if (manifest.protocol_version != kAgentBrowserRuntimeProtocolVersion) {
        return "agent browser protocol mismatch";
    }
    if (manifest.desktop_pid <= 0) return "agent browser Desktop pid is invalid";
    if (manifest.desktop_instance_id.empty()) {
        return "agent browser Desktop instance id is missing";
    }
    if (manifest.user_data_dir.empty() ||
        !path_from_utf8(manifest.user_data_dir).is_absolute()) {
        return "agent browser user data directory is invalid";
    }
#ifdef __APPLE__
    const std::filesystem::path socket_path =
        path_from_utf8(manifest.pipe_name);
    if (!socket_path.is_absolute() ||
        socket_path.filename() != "agent-browser.sock" ||
        socket_path.parent_path().filename() != "run") {
        return "agent browser proxy socket is invalid";
    }
#else
    if (manifest.pipe_name.rfind("\\\\.\\pipe\\ACECode-AgentBrowser-", 0) != 0) {
        return "agent browser proxy pipe is invalid";
    }
#endif
    if (manifest.auth_token.size() < 32 || manifest.auth_token.size() > 256) {
        return "agent browser proxy token is invalid";
    }
    if (manifest.ready_at_ms <= 0) return "agent browser ready timestamp is invalid";
    if (!pid_alive || !pid_alive(manifest.desktop_pid)) {
        return "agent browser Desktop process is not running";
    }
    return {};
}

bool cleanup_agent_browser_runtime_manifest(
    const std::string& desktop_instance_id,
    const std::string& acecode_dir) {
    if (desktop_instance_id.empty()) return false;
    const auto existing = read_agent_browser_runtime_manifest(acecode_dir);
    if (!existing.has_value()) return true;
    if (existing->desktop_instance_id != desktop_instance_id) return false;

    std::error_code ec;
    const auto path = agent_browser_runtime_manifest_path(acecode_dir);
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + ".tmp", ec);
    return !std::filesystem::exists(path, ec);
}

} // namespace acecode::desktop
