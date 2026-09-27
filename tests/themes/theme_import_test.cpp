#include <gtest/gtest.h>
#include "themes/theme_store.hpp"
#include "themes/theme_package.hpp"
#include "utils/sha256.hpp"
#include "utils/uuid.hpp"
#include "test_support/themes/theme_test_resources.hpp"
#include <fstream>

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
}
