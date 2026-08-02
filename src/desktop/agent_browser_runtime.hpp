#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace acecode::desktop {

inline constexpr int kAgentBrowserRuntimeProtocolVersion = 4;
inline constexpr std::uint32_t kAgentBrowserProxyMaxRequestBytes =
    8u * 1024u * 1024u;
inline constexpr std::uint32_t kAgentBrowserProxyMaxResponseBytes =
    64u * 1024u * 1024u;
inline constexpr std::uint8_t kAgentBrowserProxyResponseAck = 0xA5;

struct AgentBrowserRuntimeManifest {
    int protocol_version = kAgentBrowserRuntimeProtocolVersion;
    std::int64_t desktop_pid = 0;
    std::string desktop_instance_id;
    std::string user_data_dir;
    std::string pipe_name;
    std::string auth_token;
    std::int64_t ready_at_ms = 0;
};

std::filesystem::path agent_browser_root_path(
    const std::string& acecode_dir = std::string());
std::filesystem::path agent_browser_user_data_path(
    const std::string& acecode_dir = std::string());
std::filesystem::path agent_browser_macos_profile_identifier_path(
    const std::string& acecode_dir = std::string());
std::filesystem::path agent_browser_proxy_socket_path(
    const std::string& acecode_dir = std::string());
std::filesystem::path agent_browser_runtime_manifest_path(
    const std::string& acecode_dir = std::string());
// Browser address-bar policy shared by the Desktop bridge and tests. Bare
// hosts are promoted to HTTPS; privileged/local schemes are intentionally
// rejected because arbitrary pages run without ACECode bindings.
std::optional<std::string> normalize_agent_browser_url(
    const std::string& input,
    std::string* error = nullptr);

bool write_agent_browser_runtime_manifest(
    const AgentBrowserRuntimeManifest& manifest,
    const std::string& acecode_dir = std::string());
std::optional<AgentBrowserRuntimeManifest> read_agent_browser_runtime_manifest(
    const std::string& acecode_dir = std::string());

// Empty string means usable. A non-empty value is a stable diagnostic for
// Browser tools; pid_alive is injected so validation remains unit-testable.
std::string validate_agent_browser_runtime_manifest(
    const AgentBrowserRuntimeManifest& manifest,
    const std::function<bool(std::int64_t)>& pid_alive);

// Removes only a manifest owned by the supplied Desktop instance. This avoids
// a stale process deleting a newer singleton generation's endpoint.
bool cleanup_agent_browser_runtime_manifest(
    const std::string& desktop_instance_id,
    const std::string& acecode_dir = std::string());

} // namespace acecode::desktop
