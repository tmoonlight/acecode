#include <gtest/gtest.h>
#include "themes/theme_store.hpp"
#include "themes/theme_package.hpp"
#include "utils/sha256.hpp"
#include "utils/uuid.hpp"
#include "test_support/themes/theme_test_resources.hpp"
#include <fstream>
#include <chrono>

namespace {
namespace fs = std::filesystem;
using namespace acecode::themes;
using nlohmann::json;

class ThemeImportTest : public ::testing::Test {
protected:
    fs::path root;
    std::string png;
    json definition;
    void SetUp() override {
        root = fs::temp_directory_path() / ("ace-theme-import-" + acecode::generate_uuid_v7());
        fs::create_directories(root);
        png = theme_test::png();
        definition = theme_test::definition(png);
        definition["mode"] = "dark";
        definition["appearance"] = {{"logo_color", "#9864ED"}, {"home_title_color", "#EEEEFF"}, {"extend_to_titlebar", true}};
    }
    void TearDown() override { std::error_code ec; fs::remove_all(root, ec); }
    std::string package(const ThemePackageFiles& files) {
        const auto path = root / "fixture.zip";
        write_theme_archive(path, files);
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }
    std::string package() { return package({{"theme.json", definition.dump()}, {"background.png", png}, {"thumbnail.png", png}}); }
};

TEST_F(ThemeImportTest, PreviewIsReadOnlyAndConfirmedImportPreservesAppearance) {
    ThemeStore store(root / "themes", "https://unused.invalid");
    const auto bytes = package();
    const auto preview = store.preview_import(bytes);
    EXPECT_FALSE(store.installed("ai-example"));
    EXPECT_EQ(preview["theme"], definition);
    EXPECT_EQ(preview["package_sha256"], acecode::sha256_hex(bytes));
    EXPECT_EQ(preview["thumbnail_url"].get<std::string>().find("data:image/png;base64,"), 0u);
    const auto installed = store.import_archive(bytes, preview["package_sha256"]);
    EXPECT_EQ(installed["id"], "ai-example");
    EXPECT_EQ(store.definition("ai-example")["appearance"], definition["appearance"]);
    EXPECT_EQ(store.image("ai-example", "background"), png);
}

TEST_F(ThemeImportTest, ChangedBytesRequireFreshConfirmation) {
    ThemeStore store(root / "themes", "https://unused.invalid");
    auto bytes = package();
    auto digest = acecode::sha256_hex(bytes);
    definition["name"] = "Changed";
    bytes = package();
    EXPECT_THROW(store.import_archive(bytes, digest), ThemeError);
    EXPECT_THROW(store.import_archive(bytes, ""), ThemeError);
    EXPECT_FALSE(store.installed("ai-example"));
}

TEST_F(ThemeImportTest, OptionalImagesMustBeDeclaredPresentAndIntact) {
    ThemeStore store(root / "themes", "");
    ThemePackageFiles files = {{"theme.json", definition.dump()}, {"background.png", png},
        {"thumbnail.png", png}, {"session-background.png", png}};
    EXPECT_THROW(store.preview_import(package(files)), ThemeError);
    definition["session_background"] = definition["background"];
    files["theme.json"] = definition.dump();
    EXPECT_NO_THROW(store.preview_import(package(files)));
    files.erase("session-background.png");
    EXPECT_THROW(store.preview_import(package(files)), ThemeError);
    files["session-background.png"] = png + "changed";
    EXPECT_THROW(store.preview_import(package(files)), ThemeError);
    EXPECT_THROW(store.install_local(definition, png, png), ThemeError);
    EXPECT_THROW(store.install_local(definition, png, png, {{"../session-background.png", png}}), ThemeError);
    EXPECT_FALSE(store.installed("ai-example"));
}

TEST_F(ThemeImportTest, RejectsInvalidArchivesResourcesAndBuiltins) {
    ThemeStore store(root / "themes", "https://unused.invalid");
    EXPECT_THROW(store.preview_import("not a zip"), ThemeError);
    EXPECT_THROW(store.preview_import(std::string(16 * 1024 * 1024 + 1, 'x')), ThemeError);
    EXPECT_THROW(store.preview_import(package({{"theme.json", definition.dump()}, {"../background.png", png}, {"thumbnail.png", png}})), ThemeError);
    auto wrong = definition;
    wrong["background"]["sha256"] = std::string(64, '0');
    EXPECT_THROW(store.preview_import(package({{"theme.json", wrong.dump()}, {"background.png", png}, {"thumbnail.png", png}})), ThemeError);
    definition["id"] = "eva-01";
    EXPECT_THROW(store.preview_import(package()), ThemeError);
    EXPECT_FALSE(store.installed("eva-01"));
}

TEST_F(ThemeImportTest, ExistingVersionIsIdempotentButCannotBeOverwritten) {
    ThemeStore store(root / "themes", "https://unused.invalid");
    auto bytes = package();
    store.import_archive(bytes, acecode::sha256_hex(bytes));
    EXPECT_NO_THROW(store.import_archive(bytes, acecode::sha256_hex(bytes)));
    definition["name"] = "Conflicting";
    bytes = package();
    EXPECT_THROW(store.import_archive(bytes, acecode::sha256_hex(bytes)), ThemeError);
    EXPECT_NE(store.definition("ai-example")["name"], "Conflicting");
}

TEST(ThemeWorkshopAddress, UsesEffectivePortAndRejectsUnsafeAddresses) {
    EXPECT_EQ(theme_workshop_base("http://host:82/aupdate/?x=1#part"), "http://host:10082/");
    EXPECT_EQ(theme_workshop_base("http://host:1234/custom"), "http://host:11234/");
    EXPECT_EQ(theme_workshop_base("http://host/aupdate/"), "http://host:10080/");
    EXPECT_EQ(theme_workshop_base("https://host/aupdate/"), "https://host:10443/");
    EXPECT_EQ(theme_workshop_base("http://[::1]:1234/a"), "http://[::1]:11234/");
    EXPECT_EQ(theme_workshop_base("http://host:55535/a"), "http://host:65535/");
    for (const auto* url : {"", "file:///tmp", "ftp://host/a", "http://user:pass@host", "http://host:0", "http://host:55536"})
        EXPECT_THROW(theme_workshop_base(url), ThemeError) << url;
}

class ThemeWorkshopTest : public ThemeImportTest {
protected:
    json offers = json::array();
    std::string bytes;
    std::vector<std::string> urls;
    bool offline = false;
    bool corrupt = false;
    std::size_t page_size = 1;
    void SetUp() override { ThemeImportTest::SetUp(); publish(); }
    void publish() {
        bytes = package();
        const auto key = acecode::sha256_hex(bytes);
        offers.push_back({{"id", definition["id"]}, {"name", definition["name"]}, {"version", definition["version"]},
            {"key", key}, {"sha256", key}, {"bytes", bytes.size()}, {"status", "approved"}, {"mode", "dark"},
            {"swatches", {"#ABCDEF", "#FFFFFF", "#123456"}}, {"download_url", "http://untrusted.invalid/ignored"}});
    }
    ThemeTransport transport() {
        return {
            [this](const std::string& url) {
                urls.push_back(url);
                acecode::upgrade::HttpTextResult response;
                response.status_code = offline ? 503 : 200;
                if (url.find("/thumbnail") != std::string::npos) { response.body = png; return response; }
                const auto marker = url.find("api/themes?page=");
                if (marker == std::string::npos) { response.status_code = 404; return response; }
                const auto page = std::stoul(url.substr(marker + 16));
                json items = json::array();
                for (auto i = (page - 1) * page_size; i < std::min(page * page_size, offers.size()); ++i) items.push_back(offers[i]);
                response.body = json{{"themes", items}, {"total", offers.size()}, {"page", page}, {"page_size", page_size}}.dump();
                return response;
            },
            [this](const std::string& url, const fs::path& file, const acecode::upgrade::DownloadProgressCallback& progress,
                   const acecode::upgrade::HttpCancelCheck&) {
                urls.push_back(url);
                auto downloaded = corrupt ? bytes + "tampered" : bytes;
                std::ofstream stream(file, std::ios::binary);
                stream.write(downloaded.data(), downloaded.size()); stream.close();
                if (progress) progress({downloaded.size()});
                acecode::upgrade::DownloadResult result;
                result.status_code = 200; result.bytes_written = downloaded.size(); return result;
            }
        };
    }
    json offer(ThemeStore& store) {
        const auto current = store.catalog(true);
        for (const auto& entry : current["themes"])
            if (entry["id"] == "ai-example") return entry;
        return nullptr;
    }
    json install(ThemeStore& store, const json& entry) {
        store.start("ai-example", {{"confirm_download", true}, {"version", entry["version"]},
            {"bytes", entry["package"]["bytes"]}, {"sha256", entry["package"]["sha256"]}});
        for (int i = 0; i < 1000; ++i) {
            const auto job = store.job();
            if (job["state"] != "downloading" && job["state"] != "installing") return job;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ADD_FAILURE() << "Workshop installation did not finish";
        return store.job();
    }
};

TEST_F(ThemeWorkshopTest, ReadsAllPagesChoosesLatestAndBuildsOwnUrls) {
    definition["version"] = "1.1.0"; publish();
    definition["id"] = "ai-second"; publish();
    ThemeStore store(root / "themes", "http://host:82/aupdate/", transport());
    const auto catalog = store.catalog(true);
    ASSERT_EQ(catalog["themes"].size(), 3u); // Offline EVA plus two dynamic IDs.
    const auto entry = offer(store);
    EXPECT_EQ(entry["version"], "1.1.0");
    EXPECT_EQ(entry["source"], "workshop");
    EXPECT_EQ(entry["installed"], false);
    EXPECT_EQ(entry["package"]["url"], "http://host:10082/api/themes/" + offers[1]["key"].get<std::string>() + "/download");
    EXPECT_NE(std::find(urls.begin(), urls.end(), "http://host:10082/api/themes?page=3"), urls.end());
    EXPECT_EQ(store.workshop_thumbnail(offers[0]["key"]), png);
    EXPECT_THROW(store.workshop_thumbnail("../escape"), ThemeError);
}

TEST_F(ThemeWorkshopTest, InstallRefreshUpdateWithdrawalAndOfflinePreserveLocalTheme) {
    ThemeStore store(root / "themes", "http://host:82/aupdate/", transport());
    EXPECT_EQ(install(store, offer(store))["state"], "completed");
    EXPECT_EQ(store.definition("ai-example")["appearance"], definition["appearance"]);
    EXPECT_EQ(offer(store)["installed"], true);
    definition["version"] = "1.1.0"; publish();
    EXPECT_EQ(offer(store)["update_available"], true);
    EXPECT_EQ(install(store, offer(store))["state"], "completed");
    EXPECT_EQ(offer(store)["installed_version"], "1.1.0");
    offers = json::array();
    EXPECT_EQ(offer(store)["source"], "local");
    offline = true;
    EXPECT_EQ(offer(store)["installed"], true);
    EXPECT_EQ(store.image("ai-example", "thumbnail"), png);
    EXPECT_TRUE(store.catalog(true).contains("workshop_error"));
}

TEST_F(ThemeWorkshopTest, RevalidatesApprovalAndIntegrityBeforeInstallation) {
    ThemeStore store(root / "themes", "http://host:82/aupdate/", transport());
    const auto entry = offer(store);
    offers[0]["status"] = "pending";
    EXPECT_THROW(install(store, entry), ThemeError);
    EXPECT_FALSE(store.installed("ai-example"));
    offers[0]["status"] = "approved";
    corrupt = true;
    EXPECT_EQ(install(store, entry)["state"], "failed");
    EXPECT_FALSE(store.installed("ai-example"));
    corrupt = false;
    definition["version"] = "1.2.0"; publish();
    EXPECT_THROW(install(store, entry), ThemeError);
}

TEST_F(ThemeWorkshopTest, SourceSwitchAndWithdrawalCannotReuseRemoteOffers) {
    std::string base = "http://first:82/aupdate/";
    ThemeStore store(root / "themes", [&] { return base; }, transport());
    EXPECT_EQ(offer(store)["source"], "workshop");
    base = "http://second:1234/custom/";
    EXPECT_EQ(offer(store)["package"]["url"], "http://second:11234/api/themes/" + offers[0]["key"].get<std::string>() + "/download");
    offers = json::array();
    EXPECT_TRUE(offer(store).is_null());
}
}
