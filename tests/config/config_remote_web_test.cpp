#include "config/config.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

class RemoteWebConfigTempDir {
public:
    RemoteWebConfigTempDir() {
        path_ = fs::temp_directory_path() /
            ("acecode-remote-web-config-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path_);
    }

    ~RemoteWebConfigTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    fs::path config_path() const { return path_ / "config.json"; }

private:
    fs::path path_;
};

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out << text;
}

} // namespace

TEST(ConfigRemoteWeb, DefaultsKeepDaemonLocalAndProxyDisabled) {
    acecode::WebConfig web;
    EXPECT_EQ(web.bind, "127.0.0.1");
    EXPECT_FALSE(web.remote_enabled);
    EXPECT_EQ(web.remote_port, 0);
}

TEST(ConfigRemoteWeb, LegacyNonLoopbackBindMigratesToProxyIntent) {
    RemoteWebConfigTempDir temp;
    write_text(
        temp.config_path(),
        R"({"web":{"bind":"0.0.0.0","port":28080}})");

    const auto loaded = acecode::load_config_from_path(
        temp.config_path().string());
    EXPECT_EQ(loaded.web.bind, "127.0.0.1");
    EXPECT_TRUE(loaded.web.remote_enabled);

    acecode::save_config(loaded, temp.config_path().string());
    std::ifstream saved_json_input(temp.config_path(), std::ios::binary);
    ASSERT_TRUE(saved_json_input.is_open());
    const auto saved_json = nlohmann::json::parse(saved_json_input);
    ASSERT_TRUE(saved_json.contains("web"));
    EXPECT_FALSE(saved_json["web"].contains("bind"));
    EXPECT_TRUE(saved_json["web"]["remote_enabled"].get<bool>());
    const auto saved = acecode::load_config_from_path(
        temp.config_path().string());
    EXPECT_EQ(saved.web.bind, "127.0.0.1");
    EXPECT_TRUE(saved.web.remote_enabled);
}

TEST(ConfigRemoteWeb, ExplicitFlagWinsOverStaleLegacyBind) {
    RemoteWebConfigTempDir temp;
    write_text(
        temp.config_path(),
        R"({"web":{"bind":"0.0.0.0","remote_enabled":false,"remote_port":28081}})");

    const auto loaded = acecode::load_config_from_path(
        temp.config_path().string());
    EXPECT_EQ(loaded.web.bind, "127.0.0.1");
    EXPECT_FALSE(loaded.web.remote_enabled);
    EXPECT_EQ(loaded.web.remote_port, 28081);
}

TEST(ConfigRemoteWeb, LoopbackAliasesNormalizeToCanonicalDaemonBind) {
    RemoteWebConfigTempDir temp;
    write_text(
        temp.config_path(),
        R"({"web":{"bind":" ::1 ","remote_enabled":true}})");

    const auto loaded = acecode::load_config_from_path(
        temp.config_path().string());
    EXPECT_EQ(loaded.web.bind, "127.0.0.1");
    EXPECT_TRUE(loaded.web.remote_enabled);
}

TEST(ConfigRemoteWeb, ValidatesExternalPortRangeAndCollision) {
    acecode::AppConfig cfg;
    cfg.web.remote_port = 65536;
    auto errors = acecode::validate_config(cfg);
    EXPECT_TRUE(std::any_of(
        errors.begin(), errors.end(),
        [](const std::string& error) {
            return error.find("web.remote_port out of range") !=
                std::string::npos;
        }));

    cfg.web.remote_port = cfg.web.port;
    errors = acecode::validate_config(cfg);
    EXPECT_TRUE(std::any_of(
        errors.begin(), errors.end(),
        [](const std::string& error) {
            return error.find("must differ from web.port") !=
                std::string::npos;
        }));
}
