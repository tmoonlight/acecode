#include <gtest/gtest.h>
#include "themes/theme_store.hpp"
#include "utils/sha256.hpp"
#include "utils/utf8_path.hpp"
#include <chrono>
#include <fstream>
#include <map>
#include <vector>
#include <zip.h>

namespace {
namespace fs = std::filesystem;
using nlohmann::json;
using namespace acecode::themes;

class ThemeStoreTest : public ::testing::Test {
protected:
    fs::path root;
    std::string archive_bytes;
    json definition;
    json catalog;
    std::atomic<int> downloads{0};
    bool corrupt = false;
    bool offline = false;
    bool hold = false;
    bool fail_download = false;

    void SetUp() override {
        root = fs::temp_directory_path() / ("ace-theme-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
        const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(24, 'x');
        json colors = json::object();
        for (const auto* key : {"bg", "surface", "surface-alt", "surface-hi", "shell-hi", "shell-bg",
            "border", "border-soft", "fg", "fg-2", "fg-mute", "accent", "accent-bg", "accent-soft",
            "ok", "ok-bg", "ok-border", "warn", "warn-bg", "danger", "danger-bg", "code-bg", "code-fg",
            "code-line", "selection", "on-selection", "send-bg", "send-fg"}) colors[key] = "#ABCDEF";
        definition = {{"schema_version", 1}, {"id", "eva-01"}, {"version", "1.0.0"}, {"mode", "light"},
            {"colors", colors}, {"background", {{"bytes", png.size()}, {"sha256", acecode::sha256_hex(png)}}},
            {"thumbnail", {{"bytes", png.size()}, {"sha256", acecode::sha256_hex(png)}}}};
        make_archive({{"theme.json", definition.dump()}, {"background.png", png}, {"thumbnail.png", png}});
        catalog = {{"schema_version", 1}, {"themes", json::array({{
            {"id", "eva-01"}, {"version", "1.0.0"}, {"swatches", {"#ABCDEF", "#FFFFFF", "#ABCDEF"}},
            {"package", {{"path", "eva-01/1.0.0/theme.zip"}, {"bytes", archive_bytes.size()}, {"sha256", acecode::sha256_hex(archive_bytes)}}},
            {"thumbnail", {{"path", "eva-01/1.0.0/thumbnail.png"}, {"bytes", png.size()}, {"sha256", acecode::sha256_hex(png)}}}
        }})}};
    }

    void TearDown() override { std::error_code ec; fs::remove_all(root, ec); }

    void make_archive(const std::map<std::string, std::string>& files) {
        const auto path = root / "fixture.zip";
        int error = 0;
        auto* zip = zip_open(acecode::path_to_utf8(path).c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
        ASSERT_NE(zip, nullptr);
        for (const auto& [name, bytes] : files) {
            auto* source = zip_source_buffer(zip, bytes.data(), bytes.size(), 0);
            ASSERT_GE(zip_file_add(zip, name.c_str(), source, ZIP_FL_ENC_UTF_8), 0);
        }
        ASSERT_EQ(zip_close(zip), 0);
        std::ifstream stream(path, std::ios::binary);
        archive_bytes = {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

    ThemeTransport transport() {
        return {
            [this](const std::string&) {
                acecode::upgrade::HttpTextResult result;
                result.status_code = offline ? 503 : 200;
                result.body = catalog.dump();
                return result;
            },
            [this](const std::string&, const fs::path& path,
                   const acecode::upgrade::DownloadProgressCallback& progress,
                   const acecode::upgrade::HttpCancelCheck& cancel) {
                ++downloads;
                acecode::upgrade::DownloadResult result;
                if (fail_download) { result.status_code = 503; return result; }
                while (hold && !cancel()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                if (cancel()) { result.cancelled = true; return result; }
                std::ofstream stream(path, std::ios::binary);
                auto bytes = corrupt ? archive_bytes + "corruption" : archive_bytes;
                stream.write(bytes.data(), bytes.size()); stream.close();
                if (progress) progress({bytes.size()});
                result.status_code = 200;
                result.bytes_written = bytes.size();
                return result;
            }
        };
    }

    json consent() {
        auto e = catalog["themes"][0];
        return {{"confirm_download", true}, {"version", e["version"]},
            {"bytes", e["package"]["bytes"]}, {"sha256", e["package"]["sha256"]}};
    }

    json finish(ThemeStore& store) {
        for (int i = 0; i < 500; ++i) {
            auto job = store.job();
            if (job["state"] != "downloading" && job["state"] != "installing") return job;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ADD_FAILURE() << "Theme job did not finish";
        return store.job();
    }
};

TEST_F(ThemeStoreTest, CatalogAndDefinitionRejectUnsafePathsAndExecutableValues) {
    EXPECT_TRUE(valid_theme_catalog(catalog));
    EXPECT_TRUE(valid_theme_definition(definition));
    auto invalid = catalog;
    invalid["themes"][0]["package"]["path"] = "https://example.com/theme.zip";
    EXPECT_FALSE(valid_theme_catalog(invalid));
    invalid = catalog; invalid["themes"][0]["version"] = "../outside";
    EXPECT_FALSE(valid_theme_catalog(invalid));
    auto theme = definition; theme["colors"]["bg"] = "url(https://example.com)";
    EXPECT_FALSE(valid_theme_definition(theme));
    theme = definition; theme["colors"]["unknown"] = "#FFFFFF";
    EXPECT_FALSE(valid_theme_definition(theme));
    theme = definition; theme["mode"] = "dark";
    EXPECT_FALSE(valid_theme_definition(theme));
}

TEST_F(ThemeStoreTest, AppearanceAcceptsOnlyOptionalHexColorsAndBooleanTitlebarExtension) {
    EXPECT_TRUE(valid_theme_definition(definition));
    for (const auto& appearance : std::vector<json>{
             json::object(), {{"logo_color", "#aBcDeF"}}, {{"home_title_color", "#012345"}},
             {{"extend_to_titlebar", false}},
             {{"logo_color", "#9B6DFF"}, {"home_title_color", "#FFFFFF"}, {"extend_to_titlebar", true}}}) {
        auto theme = definition;
        theme["appearance"] = appearance;
        EXPECT_TRUE(valid_theme_definition(theme)) << appearance;
        theme["id"] = "ai-example";
        theme["name"] = "Example";
        theme["mode"] = "dark";
        EXPECT_TRUE(valid_theme_definition(theme)) << appearance;
    }
    for (const auto& appearance : std::vector<json>{
             nullptr, true, 1, "#FFFFFF", json::array(), {{"unknown", true}},
             {{"logo_color", nullptr}}, {{"logo_color", "#FFF"}}, {{"logo_color", "#FFFFFF80"}},
             {{"logo_color", "#GGGGGG"}}, {{"logo_color", "var(--accent)"}},
             {{"home_title_color", 123}}, {{"home_title_color", "url(https://example.com)"}},
             {{"extend_to_titlebar", "true"}}, {{"extend_to_titlebar", 1}},
             {{"extend_to_titlebar", nullptr}}}) {
        auto theme = definition;
        theme["appearance"] = appearance;
        EXPECT_FALSE(valid_theme_definition(theme)) << appearance;
    }
}

TEST_F(ThemeStoreTest, NationalDayDownloadsByIdAndPreservesItsApprovedMode) {
    definition["id"] = kNationalDayThemeId;
    definition["name"] = "National Day";
    definition["mode"] = "dark";
    EXPECT_TRUE(valid_theme_definition(definition));
    const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(24, 'x');
    make_archive({{"theme.json", definition.dump()}, {"background.png", png}, {"thumbnail.png", png}});
    auto entry = catalog["themes"][0];
    entry["id"] = kNationalDayThemeId;
    entry["package"] = {{"path", "national-day-2026/1.0.0/theme.zip"},
        {"bytes", archive_bytes.size()}, {"sha256", acecode::sha256_hex(archive_bytes)}};
    entry["thumbnail"]["path"] = "national-day-2026/1.0.0/thumbnail.png";
    catalog["themes"].push_back(entry); // EVA first must not redirect the requested ID.
    ASSERT_TRUE(valid_theme_catalog(catalog));
    auto invalid = catalog;
    invalid["themes"][1] = invalid["themes"][0];
    EXPECT_FALSE(valid_theme_catalog(invalid));
    invalid = catalog; invalid["themes"][1]["package"]["path"] = "eva-01/1.0.0/theme.zip";
    EXPECT_FALSE(valid_theme_catalog(invalid));
    invalid = catalog; invalid["themes"][1]["id"] = "unknown";
    EXPECT_FALSE(valid_theme_catalog(invalid));

    ThemeStore store(root / "cache", "https://example.com/aupdate/", transport());
    auto permission = json{{"confirm_download", true}, {"version", entry["version"]},
        {"bytes", entry["package"]["bytes"]}, {"sha256", entry["package"]["sha256"]}, {"automatic", true}};
    store.start(kNationalDayThemeId, permission);
    const auto completed = finish(store);
    EXPECT_EQ(completed["state"], "completed");
    EXPECT_EQ(completed["automatic"], true);
    EXPECT_EQ(store.definition(kNationalDayThemeId)["mode"], "dark");
    EXPECT_FALSE(store.installed("eva-01"));
    EXPECT_THROW(store.remove_local(kNationalDayThemeId), ThemeError);
    EXPECT_THROW(store.start_export(kNationalDayThemeId), ThemeError);
    offline = true;
    ThemeStore restarted(root / "cache", "https://another.example/aupdate/", transport());
    const auto themes = restarted.catalog()["themes"];
    EXPECT_EQ(themes[0]["id"], kNationalDayThemeId);
    EXPECT_EQ(themes[0]["installed"], true);
}

TEST_F(ThemeStoreTest, LegacyMirrorFallsBackOnlyWhenExpandedCatalogIsMissing) {
    auto io = transport();
    std::vector<std::string> urls;
    io.fetch = [&](const std::string& url) {
        urls.push_back(url);
        acecode::upgrade::HttpTextResult result;
        result.status_code = url.find("catalog-v2.json") != std::string::npos ? 404 : 200;
        result.body = catalog.dump();
        return result;
    };
    ThemeStore store(root / "cache", "https://example.com/aupdate/", io);
    EXPECT_EQ(store.catalog()["themes"][0]["id"], "eva-01");
    ASSERT_EQ(urls.size(), 3u);
    EXPECT_EQ(urls[0], "https://example.com/aupdate/themes/catalog-v2.json");
    EXPECT_EQ(urls[1], "https://example.com/aupdate/themes/catalog.json");
    EXPECT_EQ(urls[2], "https://example.com:10443/api/themes?page=1");
    EXPECT_EQ(downloads.load(), 0);
}

TEST_F(ThemeStoreTest, StartupAttemptIsClaimedOnceAcrossConcurrentStoresAndRestarts) {
    ThemeStore first(root / "cache", "https://example.com/", transport());
    ThemeStore second(root / "cache", "https://example.com/", transport());
    std::atomic<int> claims{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < 8; ++i) callers.emplace_back([&, i] {
        const auto result = (i % 2 ? first : second).claim_startup_theme();
        if (result.at("claimed") == true) ++claims;
    });
    for (auto& caller : callers) caller.join();
    EXPECT_EQ(claims.load(), 1);
    ThemeStore restarted(root / "cache", "https://different.example/", transport());
    EXPECT_EQ(restarted.claim_startup_theme()["claimed"], false);
    EXPECT_EQ(downloads.load(), 0);
    ThemeStore other_profile(root / "other", "https://example.com/", transport());
    EXPECT_EQ(other_profile.claim_startup_theme()["claimed"], true);
    std::ofstream(root / "blocked").put('x');
    ThemeStore blocked(root / "blocked", "https://example.com/", transport());
    EXPECT_THROW(blocked.claim_startup_theme(), ThemeError);
}

TEST_F(ThemeStoreTest, NoDownloadUntilExactSizeAndIdentityAreConfirmed) {
    ThemeStore store(root / "cache", "https://example.com/aupdate/", transport());
    EXPECT_FALSE(store.catalog()["themes"][0]["installed"]);
    EXPECT_EQ(downloads.load(), 0);
    EXPECT_THROW(store.start("eva-01", json::object()), ThemeError);
    auto wrong = consent(); wrong["bytes"] = 1;
    EXPECT_THROW(store.start("eva-01", wrong), ThemeError);
    wrong = consent(); wrong["sha256"] = std::string(64, '0');
    EXPECT_THROW(store.start("eva-01", wrong), ThemeError);
    EXPECT_EQ(downloads.load(), 0);
}

TEST_F(ThemeStoreTest, CatalogFailureReportsTheConfiguredMirrorAddress) {
    offline = true;
    ThemeStore store(root / "cache", "https://mirror.example/custom/", transport());
    try {
        store.catalog(true);
        FAIL() << "Expected catalogue failure";
    } catch (const ThemeError& error) {
        EXPECT_EQ(error.code, "THEME_CATALOG_UNAVAILABLE");
        EXPECT_EQ(error.path, "https://mirror.example/custom/themes/catalog-v2.json");
        EXPECT_NE(std::string(error.what()).find("503"), std::string::npos);
    }
    EXPECT_EQ(downloads.load(), 0);
}

TEST_F(ThemeStoreTest, CurrentServerReplacesStartupAddressAndScopesCachedCatalog) {
    std::string base = "https://old.example/aupdate/";
    ThemeStore store(root / "cache", [&] { return base; }, transport());
    offline = true;
    EXPECT_THROW(store.catalog(), ThemeError);

    base = "http://new.example:82/aupdate/";
    offline = false;
    EXPECT_EQ(store.catalog()["themes"][0]["package"]["url"],
              base + "themes/eva-01/1.0.0/theme.zip");

    offline = true;
    ThemeStore same_server(root / "cache", base, transport());
    EXPECT_TRUE(same_server.catalog(true)["offline"]);
    base = "https://old.example/aupdate/";
    try {
        store.catalog();
        FAIL() << "A different server must not reuse the previous catalogue";
    } catch (const ThemeError& error) {
        EXPECT_EQ(error.path, base + "themes/catalog-v2.json");
    }
    ThemeStore different_server(root / "cache", base, transport());
    EXPECT_THROW(different_server.catalog(), ThemeError);
    EXPECT_EQ(downloads.load(), 0);
}

TEST_F(ThemeStoreTest, ActiveDownloadRetainsConfirmedSourceAfterServerChange) {
    std::string base = "https://old.example/aupdate/";
    auto io = transport();
    const auto download = io.download;
    std::atomic<bool> started{false};
    std::atomic<bool> wait_for_switch{true};
    std::vector<std::string> download_urls;
    io.download = [&](const std::string& url, const fs::path& path,
                      const acecode::upgrade::DownloadProgressCallback& progress,
                      const acecode::upgrade::HttpCancelCheck& cancel) {
        download_urls.push_back(url);
        started.store(true);
        while (wait_for_switch.load() && !cancel()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return download(url, path, progress, cancel);
    };
    ThemeStore store(root / "cache", [&] { return base; }, std::move(io));
    store.start("eva-01", consent());
    for (int i = 0; i < 500 && !started.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(started.load());
    base = "http://new.example:82/aupdate/";
    const auto entry = store.catalog()["themes"][0];
    EXPECT_EQ(entry["package"]["url"], base + "themes/eva-01/1.0.0/theme.zip");
    EXPECT_EQ(entry["thumbnail"]["url"], base + "themes/eva-01/1.0.0/thumbnail.png");
    wait_for_switch.store(false);
    ASSERT_EQ(finish(store)["state"], "completed");
    store.start("eva-01", consent());
    ASSERT_EQ(finish(store)["state"], "completed");
    ASSERT_EQ(download_urls.size(), 2u);
    EXPECT_EQ(download_urls[0], "https://old.example/aupdate/themes/eva-01/1.0.0/theme.zip");
    EXPECT_EQ(download_urls[1], base + "themes/eva-01/1.0.0/theme.zip");
}

TEST_F(ThemeStoreTest, DownloadFailureReportsArchiveAddressAndSuccessfulRetryClearsIt) {
    ThemeStore store(root / "cache", "https://mirror.example/custom/", transport());
    EXPECT_EQ(store.catalog()["themes"][0]["package"]["url"],
              "https://mirror.example/custom/themes/eva-01/1.0.0/theme.zip");
    fail_download = true;
    store.start("eva-01", consent());
    const auto failed = finish(store);
    EXPECT_EQ(failed["error"], "THEME_DOWNLOAD_FAILED");
    EXPECT_EQ(failed["error_path"], "https://mirror.example/custom/themes/eva-01/1.0.0/theme.zip");
    EXPECT_FALSE(store.installed("eva-01"));
    fail_download = false;
    store.start("eva-01", consent());
    const auto completed = finish(store);
    EXPECT_EQ(completed["state"], "completed");
    EXPECT_FALSE(completed.contains("error_path"));
}

TEST_F(ThemeStoreTest, VerifiedInstallationSurvivesOfflineRestartWithoutRedownload) {
    {
        ThemeStore store(root / "cache", "https://example.com/aupdate/", transport());
        store.start("eva-01", consent());
        EXPECT_EQ(finish(store)["state"], "completed");
        EXPECT_EQ(store.definition("eva-01"), definition);
        EXPECT_EQ(store.image("eva-01", "background").size(), 32);
    }
    offline = true;
    ThemeStore reopened(root / "cache", "https://example.com/aupdate/", transport());
    EXPECT_TRUE(reopened.installed("eva-01"));
    EXPECT_TRUE(reopened.catalog(true)["offline"]);
    EXPECT_EQ(reopened.definition("eva-01")["mode"], "light");
    EXPECT_EQ(downloads.load(), 1);
}

TEST_F(ThemeStoreTest, CorruptedDownloadIsNotInstalledAndCanBeRetried) {
    ThemeStore store(root / "cache", "https://example.com/aupdate/", transport());
    corrupt = true;
    store.start("eva-01", consent());
    EXPECT_EQ(finish(store)["error"], "THEME_INVALID_PACKAGE");
    EXPECT_FALSE(store.installed("eva-01"));
    corrupt = false;
    store.start("eva-01", consent());
    EXPECT_EQ(finish(store)["state"], "completed");
}

TEST_F(ThemeStoreTest, CancellationAndConcurrentInstallRetainNoPartialTheme) {
    ThemeStore store(root / "cache", "https://example.com/aupdate/", transport());
    hold = true;
    store.start("eva-01", consent());
    EXPECT_THROW(store.start("eva-01", consent()), ThemeError);
    store.cancel();
    EXPECT_EQ(finish(store)["state"], "cancelled");
    EXPECT_FALSE(store.installed("eva-01"));
}

TEST_F(ThemeStoreTest, RedownloadRepairsDamagedInstalledResources) {
    ThemeStore store(root / "cache", "https://example.com/aupdate/", transport());
    store.start("eva-01", consent());
    ASSERT_EQ(finish(store)["state"], "completed");
    {
        std::ofstream damaged(root / "cache/eva-01/1.0.0/background.png", std::ios::binary);
        damaged << "damaged";
    }
    EXPECT_FALSE(store.installed("eva-01"));
    store.start("eva-01", consent());
    EXPECT_EQ(finish(store)["state"], "completed");
    EXPECT_EQ(store.definition("eva-01"), definition);
    EXPECT_EQ(downloads.load(), 2);
}

TEST_F(ThemeStoreTest, AuthenticatedHashCannotAuthorizeAnUnsafeArchiveEntry) {
    make_archive({{"../outside", "evil"}, {"theme.json", definition.dump()}, {"thumbnail.png", "x"}});
    catalog["themes"][0]["package"]["bytes"] = archive_bytes.size();
    catalog["themes"][0]["package"]["sha256"] = acecode::sha256_hex(archive_bytes);
    ThemeStore store(root / "cache", "https://example.com/aupdate/", transport());
    store.start("eva-01", consent());
    EXPECT_EQ(finish(store)["error"], "THEME_INVALID_PACKAGE");
    EXPECT_FALSE(fs::exists(root / "outside"));
}

TEST_F(ThemeStoreTest, NewVersionRequiresConsentAndPreservesOldResourcesUntilSuccessfulUpdate) {
    ThemeStore store(root / "cache", "https://example.com/aupdate/", transport());
    store.start("eva-01", consent());
    ASSERT_EQ(finish(store)["state"], "completed");
    const auto old_consent = consent();
    const auto old_definition = definition;
    const auto old_catalog = catalog;
    const auto png = store.image("eva-01", "background");
    definition["version"] = "1.0.1";
    make_archive({{"theme.json", definition.dump()}, {"background.png", png}, {"thumbnail.png", png}});
    auto& e = catalog["themes"][0];
    e["version"] = "1.0.1";
    e["package"] = {{"path", "eva-01/1.0.1/theme.zip"}, {"bytes", archive_bytes.size()},
                    {"sha256", acecode::sha256_hex(archive_bytes)}};
    e["thumbnail"]["path"] = "eva-01/1.0.1/thumbnail.png";
    const auto available = store.catalog(true)["themes"][0];
    EXPECT_TRUE(available["installed"]);
    EXPECT_EQ(available["installed_version"], "1.0.0");
    EXPECT_TRUE(available["update_available"]);
    EXPECT_EQ(downloads.load(), 1);
    EXPECT_THROW(store.start("eva-01", old_consent), ThemeError);
    EXPECT_EQ(downloads.load(), 1);

    corrupt = true;
    store.start("eva-01", consent());
    ASSERT_EQ(finish(store)["state"], "failed");
    EXPECT_EQ(store.definition("eva-01"), old_definition);
    EXPECT_TRUE(store.catalog()["themes"][0]["update_available"]);
    corrupt = false;
    store.start("eva-01", consent());
    ASSERT_EQ(finish(store)["state"], "completed");
    EXPECT_EQ(store.definition("eva-01"), definition);
    EXPECT_EQ(store.catalog()["themes"][0]["installed_version"], "1.0.1");
    EXPECT_FALSE(store.catalog()["themes"][0]["update_available"]);
    EXPECT_TRUE(fs::is_regular_file(root / "cache/eva-01/1.0.0/background.png"));

    catalog = old_catalog;
    EXPECT_FALSE(store.catalog(true)["themes"][0]["update_available"]);
    EXPECT_EQ(store.definition("eva-01")["version"], "1.0.1");
}
} // namespace
