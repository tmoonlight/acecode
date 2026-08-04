#include <gtest/gtest.h>

#include "desktop/agent_browser_runtime.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <random>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using namespace acecode::desktop;

namespace {

class TempDir {
public:
    TempDir() {
        std::mt19937 rng(std::random_device{}());
        for (int attempt = 0; attempt < 8; ++attempt) {
            fs::path candidate = fs::temp_directory_path() /
                ("acecode-agent-browser-runtime-" + std::to_string(rng()));
            std::error_code ec;
            if (fs::create_directories(candidate, ec) && !ec) {
                path_ = std::move(candidate);
                break;
            }
        }
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

AgentBrowserRuntimeManifest valid_manifest(const fs::path& root) {
    return AgentBrowserRuntimeManifest{
        kAgentBrowserRuntimeProtocolVersion,
        4242,
        "desktop-instance-a",
#ifdef __APPLE__
        acecode::path_to_utf8(root / "agent-browser"),
        acecode::path_to_utf8(root / "run" / "agent-browser.sock"),
#else
        acecode::path_to_utf8(root / "agent-browser" / "webview2"),
        "\\\\.\\pipe\\ACECode-AgentBrowser-4242-desktop-instance-a",
#endif
        "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG",
        1234567,
    };
}

} // namespace

TEST(AgentBrowserRuntime, PathsKeepPersistentProfileSeparateFromRuntimeFile) {
    TempDir dir;
    EXPECT_EQ(agent_browser_user_data_path(acecode::path_to_utf8(dir.path())),
              dir.path() / "agent-browser" / "webview2");
    EXPECT_EQ(agent_browser_macos_profile_identifier_path(
                  acecode::path_to_utf8(dir.path())),
              dir.path() / "agent-browser" / "macos-profile-id");
    EXPECT_EQ(agent_browser_proxy_socket_path(
                  acecode::path_to_utf8(dir.path())),
              dir.path() / "run" / "agent-browser.sock");
    EXPECT_EQ(agent_browser_runtime_manifest_path(acecode::path_to_utf8(dir.path())),
              dir.path() / "run" / "agent-browser.json");
}

TEST(AgentBrowserRuntime, NormalizesOnlySafeWebAddresses) {
    std::string error;
    EXPECT_EQ(normalize_agent_browser_url(" example.com/path ", &error),
              std::optional<std::string>("https://example.com/path"));
    EXPECT_EQ(normalize_agent_browser_url("HTTP://localhost:3000", &error),
              std::optional<std::string>("HTTP://localhost:3000"));
    EXPECT_EQ(normalize_agent_browser_url("about:blank", &error),
              std::optional<std::string>("about:blank"));
    EXPECT_EQ(normalize_agent_browser_url("webview2 agent browser", &error),
              std::optional<std::string>(
                  "https://www.bing.com/search?q=webview2%20agent%20browser"));
    EXPECT_FALSE(normalize_agent_browser_url("file:///C:/secret.txt", &error));
    EXPECT_EQ(error, "browser URL scheme is not allowed");
    EXPECT_FALSE(normalize_agent_browser_url("javascript:alert(1)", &error));
    EXPECT_FALSE(normalize_agent_browser_url("   ", &error));
    EXPECT_EQ(error, "browser URL is empty");
}

TEST(AgentBrowserRuntime, ManifestRoundTripsAndValidatesOwner) {
    TempDir dir;
    const std::string root = acecode::path_to_utf8(dir.path());
    const auto expected = valid_manifest(dir.path());
    ASSERT_TRUE(write_agent_browser_runtime_manifest(expected, root));

    std::ifstream manifest_file(agent_browser_runtime_manifest_path(root));
    nlohmann::json manifest_json;
    manifest_file >> manifest_json;
    EXPECT_EQ(manifest_json.value("protocol_version", 0), 4);
    EXPECT_FALSE(manifest_json.contains("page_id"));

    const auto actual = read_agent_browser_runtime_manifest(root);
    ASSERT_TRUE(actual.has_value());
    EXPECT_EQ(actual->protocol_version, expected.protocol_version);
    EXPECT_EQ(actual->desktop_pid, expected.desktop_pid);
    EXPECT_EQ(actual->desktop_instance_id, expected.desktop_instance_id);
    EXPECT_EQ(actual->user_data_dir, expected.user_data_dir);
    EXPECT_EQ(actual->pipe_name, expected.pipe_name);
    EXPECT_EQ(actual->auth_token, expected.auth_token);
    EXPECT_EQ(actual->ready_at_ms, expected.ready_at_ms);
    EXPECT_TRUE(validate_agent_browser_runtime_manifest(
        *actual, [](std::int64_t pid) { return pid == 4242; }).empty());
}

TEST(AgentBrowserRuntime, ValidationRejectsStaleOrMalformedEndpoint) {
    TempDir dir;
    auto manifest = valid_manifest(dir.path());
    EXPECT_FALSE(validate_agent_browser_runtime_manifest(
        manifest, [](std::int64_t) { return false; }).empty());

    manifest.protocol_version += 1;
    EXPECT_NE(validate_agent_browser_runtime_manifest(
                  manifest, [](std::int64_t) { return true; })
                  .find("protocol mismatch"),
              std::string::npos);

    manifest = valid_manifest(dir.path());
    manifest.pipe_name = "not-a-pipe";
#ifdef __APPLE__
    EXPECT_NE(validate_agent_browser_runtime_manifest(
                  manifest, [](std::int64_t) { return true; })
                  .find("proxy socket"),
              std::string::npos);
#else
    EXPECT_NE(validate_agent_browser_runtime_manifest(
                  manifest, [](std::int64_t) { return true; })
                  .find("proxy pipe"),
              std::string::npos);
#endif
}

TEST(AgentBrowserRuntime, CleanupCannotDeleteNewerDesktopGeneration) {
    TempDir dir;
    const std::string root = acecode::path_to_utf8(dir.path());
    ASSERT_TRUE(write_agent_browser_runtime_manifest(valid_manifest(dir.path()), root));

    EXPECT_FALSE(cleanup_agent_browser_runtime_manifest("desktop-instance-old", root));
    EXPECT_TRUE(read_agent_browser_runtime_manifest(root).has_value());
    EXPECT_TRUE(cleanup_agent_browser_runtime_manifest("desktop-instance-a", root));
    EXPECT_FALSE(read_agent_browser_runtime_manifest(root).has_value());
}
