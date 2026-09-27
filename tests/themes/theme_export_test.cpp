#include <gtest/gtest.h>
#include "themes/theme_store.hpp"
#include "themes/theme_package.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"
#include "test_support/themes/theme_test_resources.hpp"

#include <fstream>
#include <future>
#include <zip.h>

namespace {
namespace fs = std::filesystem;
using nlohmann::json;
using namespace acecode::themes;

class ThemeExportTest : public ::testing::Test {
protected:
    fs::path root;
    std::string png;
    json definition;
    void SetUp() override {
        root = fs::temp_directory_path() / ("ace-theme-export-" + acecode::generate_uuid_v7());
        fs::create_directories(root);
        png = theme_test::png();
        definition = theme_test::definition(png);
    }
    void TearDown() override { std::error_code ec; fs::remove_all(root, ec); }
    json finish(ThemeStore& store, const json& started) {
        const auto id = started.at("job_id").get<std::string>();
        for (int i = 0; i < 2000; ++i) {
            const auto job = store.export_job(id);
            if (job["state"] == "completed" || job["state"] == "cancelled" || job["state"] == "failed") return job;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ADD_FAILURE() << "Theme export did not finish";
        return store.export_job(id);
    }
    std::string read(const fs::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }
};

TEST_F(ThemeExportTest, ReusesOnlyValidatedZipAndRebuildsMissingOrStalePackages) {
    ThemeStore store(root, "https://unused.invalid");
    const auto installed = store.install_local(definition, png, png);
    const auto package = acecode::path_from_utf8(installed.at("package_path"));
    EXPECT_EQ(package, root / "exports/ai-example/1.0.0.zip");
    auto completed = finish(store, store.start_export("ai-example"));
    ASSERT_EQ(completed["state"], "completed") << completed;
    EXPECT_TRUE(completed["reused"]);
    EXPECT_FALSE(completed["native_saved"]);
    EXPECT_EQ(store.export_download(completed["job_id"]), read(package));
    EXPECT_NE(completed["filename"].get<std::string>().find("初号机 _ 自定义"), std::string::npos);

    fs::remove(package);
    completed = finish(store, store.start_export("ai-example"));
    ASSERT_EQ(completed["state"], "completed") << completed;
    EXPECT_FALSE(completed["reused"]);
    auto* archive = zip_open(acecode::path_to_utf8(package).c_str(), ZIP_RDONLY, nullptr);
    ASSERT_NE(archive, nullptr);
    EXPECT_EQ(zip_get_num_entries(archive, 0), 3);
    for (const auto* name : {"theme.json", "background.png", "thumbnail.png"}) EXPECT_GE(zip_name_locate(archive, name, 0), 0);
    zip_close(archive);

    std::ofstream(package, std::ios::binary | std::ios::trunc) << "stale";
    completed = finish(store, store.start_export("ai-example"));
    EXPECT_EQ(completed["state"], "completed") << completed;
    EXPECT_FALSE(completed["reused"]);
    EXPECT_EQ(store.definition("ai-example"), definition);
}

TEST_F(ThemeExportTest, RebuiltMultiBackgroundPackageRoundTripsAllResourcesAndOriginalAppearance) {
    definition["session_background"] = definition["background"];
    definition["user_message_background"] = definition["background"];
    definition["appearance"] = {{"logo_color", "#9864ED"}, {"home_title_color", "#EEEEFF"},
        {"extend_to_titlebar", true}, {"home_composer_opacity", 0.7}, {"session_background_opacity", 0.4}};
    ThemeStore store(root, "");
    const auto installed = store.install_local(definition, png, png,
        {{"session-background.png", png}, {"user-message-background.png", png}});
    fs::remove(acecode::path_from_utf8(installed.at("package_path")));
    const auto completed = finish(store, store.start_export("ai-example"));
    ASSERT_EQ(completed["state"], "completed") << completed;
    EXPECT_FALSE(completed["reused"]);
    const auto bytes = store.export_download(completed.at("job_id"));
    ThemeStore imported(root / "imported", "");
    const auto preview = imported.preview_import(bytes);
    EXPECT_EQ(preview.at("theme"), definition);
    imported.import_archive(bytes, preview.at("package_sha256"));
    EXPECT_EQ(imported.definition("ai-example"), definition);
    EXPECT_EQ(imported.image("ai-example", "session-background"), png);
    EXPECT_EQ(imported.image("ai-example", "user-message-background"), png);
    EXPECT_TRUE(imported.remove_local("ai-example")["deleted"]);
    EXPECT_FALSE(imported.installed("ai-example"));
}

TEST_F(ThemeExportTest, AppearanceRoundTripsAndInvalidatesAnOtherwiseMatchingCachedPackage) {
    definition["appearance"] = {{"logo_color", "#9B6DFF"}, {"home_title_color", "#FFFFFF"},
        {"extend_to_titlebar", true}};
    ThemeStore store(root, "https://unused.invalid");
    const auto installed = store.install_local(definition, png, png);
    const auto package = acecode::path_from_utf8(installed.at("package_path"));
    auto stale = definition;
    stale["appearance"]["extend_to_titlebar"] = false;
    write_theme_archive(package, {{"theme.json", stale.dump(2)}, {"background.png", png}, {"thumbnail.png", png}});
    const auto completed = finish(store, store.start_export("ai-example"));
    ASSERT_EQ(completed["state"], "completed") << completed;
    EXPECT_FALSE(completed["reused"]);
    EXPECT_EQ(store.export_download(completed.at("job_id")), read(package));

    std::unique_ptr<zip_t, decltype(&zip_discard)> archive(
        zip_open(acecode::path_to_utf8(package).c_str(), ZIP_RDONLY, nullptr), zip_discard);
    ASSERT_NE(archive, nullptr);
    EXPECT_EQ(zip_get_num_entries(archive.get(), 0), 3);
    zip_stat_t info{};
    ASSERT_EQ(zip_stat(archive.get(), "theme.json", 0, &info), 0);
    std::unique_ptr<zip_file_t, decltype(&zip_fclose)> manifest(
        zip_fopen(archive.get(), "theme.json", 0), zip_fclose);
    ASSERT_NE(manifest, nullptr);
    std::string bytes(static_cast<std::size_t>(info.size), '\0');
    ASSERT_EQ(zip_fread(manifest.get(), bytes.data(), bytes.size()), static_cast<zip_int64_t>(bytes.size()));
    const auto exported = json::parse(bytes);
    EXPECT_EQ(exported, definition);
    ThemeStore imported(root / "imported", "");
    imported.install_local(exported, png, png);
    EXPECT_EQ(imported.definition("ai-example"), definition);
}

TEST_F(ThemeExportTest, ReusesLegacyFlatPackageButDeletionChecksItsOwner) {
    ThemeStore store(root, "https://unused.invalid");
    const auto installed = store.install_local(definition, png, png);
    const auto package = acecode::path_from_utf8(installed.at("package_path"));
    const auto legacy = root / "exports/ai-example-1.0.0.zip";
    fs::rename(package, legacy);
    const auto completed = finish(store, store.start_export("ai-example"));
    EXPECT_TRUE(completed["reused"]);
    EXPECT_EQ(store.export_download(completed["job_id"]), read(legacy));
    EXPECT_TRUE(store.remove_local("ai-example")["deleted"]);
    EXPECT_FALSE(fs::exists(legacy));
}

TEST_F(ThemeExportTest, DamagedArchiveCrcTriggersRebuild) {
    ThemeStore store(root, "https://unused.invalid");
    const auto installed = store.install_local(definition, png, png);
    const auto package = acecode::path_from_utf8(installed.at("package_path"));
    auto bytes = read(package);
    for (const auto& [signature, offset] : std::vector<std::pair<std::string, std::size_t>>{
             {std::string("PK\x03\x04", 4), 14}, {std::string("PK\x01\x02", 4), 16}}) {
        for (std::size_t at = bytes.find(signature); at != std::string::npos; at = bytes.find(signature, at + 4)) {
            ASSERT_LT(at + offset, bytes.size());
            bytes[at + offset] ^= 0x55;
        }
    }
    std::ofstream output(package, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), bytes.size()); output.close();
    const auto completed = finish(store, store.start_export("ai-example"));
    ASSERT_EQ(completed["state"], "completed") << completed;
    EXPECT_FALSE(completed["reused"]);
    auto* archive = zip_open(acecode::path_to_utf8(package).c_str(), ZIP_RDONLY, nullptr);
    ASSERT_NE(archive, nullptr);
    for (zip_uint64_t index = 0; index < 3; ++index) {
        auto* entry = zip_fopen_index(archive, index, 0);
        ASSERT_NE(entry, nullptr);
        char buffer[1024];
        zip_int64_t count;
        do { count = zip_fread(entry, buffer, sizeof(buffer)); } while (count > 0);
        EXPECT_EQ(count, 0);
        zip_fclose(entry);
    }
    zip_close(archive);
}

TEST_F(ThemeExportTest, ConcurrentCompletedJobReclamationKeepsRegisteredWorkerHandles) {
    ThemeStore store(root, "https://unused.invalid");
    for (int index = 0; index < 80; ++index)
        store.install_local(theme_test::definition(png, "ai-example-" + std::to_string(index)), png, png);
    for (int index = 0; index < 32; ++index)
        ASSERT_EQ(finish(store, store.start_export("ai-example-" + std::to_string(index)))["state"], "completed");
    std::vector<std::future<json>> starts;
    for (int index = 32; index < 80; ++index) starts.push_back(std::async(std::launch::async, [&, index] {
        try { return store.start_export("ai-example-" + std::to_string(index)); }
        catch (const ThemeError& error) { return json{{"error", error.code}}; }
    }));
    std::size_t started = 0;
    for (auto& future : starts) {
        const auto result = future.get();
        if (result.contains("job_id")) ++started;
        else EXPECT_EQ(result.value("error", ""), "THEME_BUSY");
    }
    EXPECT_GE(started, 32u);
    // Destruction joins all retained workers; fast pruned workers were joined
    // before reclamation. No detached joinable thread may outlive its job.
}

TEST_F(ThemeExportTest, CatalogWaitsForDeleteRollbackInsteadOfObservingIsolation) {
    ThemeTransport offline;
    offline.fetch = [](const std::string&) { return acecode::upgrade::HttpTextResult{503}; };
    ThemeStore store(root, "https://unused.invalid", offline);
    ThemeStore observer(root, "https://unused.invalid", offline);
    store.install_local(definition, png, png);
    std::promise<void> entered, release, catalog_started;
    auto gate = release.get_future().share();
    auto deletion = std::async(std::launch::async, [&] {
        try { store.remove_local("ai-example", [&] { entered.set_value(); gate.wait(); throw ThemeError(500, "PERSIST_FAILED", "test"); }); }
        catch (const ThemeError& error) { return error.code; }
        return std::string("unexpected success");
    });
    entered.get_future().wait();
    auto catalog = std::async(std::launch::async, [&] { catalog_started.set_value(); return observer.catalog(); });
    catalog_started.get_future().wait();
    const auto blocked = catalog.wait_for(std::chrono::milliseconds(30));
    release.set_value();
    EXPECT_EQ(blocked, std::future_status::timeout);
    EXPECT_EQ(deletion.get(), "PERSIST_FAILED");
    const auto restored = catalog.get();
    ASSERT_EQ(restored["themes"].size(), 2u);
    EXPECT_EQ(restored["themes"][1]["id"], "ai-example");
}

TEST_F(ThemeExportTest, CombinedIsolationValidationPrecedesPreferenceCommit) {
    ThemeStore store(root, "https://unused.invalid");
    store.install_local(definition, png, png);
    for (int index = 0; index < 4090; ++index)
        std::ofstream(root / "ai-example" / ("extra-" + std::to_string(index)));
    bool committed = false;
    EXPECT_THROW(store.remove_local("ai-example", [&] { committed = true; }), ThemeError);
    EXPECT_FALSE(committed);
    EXPECT_EQ(store.definition("ai-example"), definition);
    EXPECT_TRUE(fs::exists(root / "exports/ai-example/1.0.0.zip"));
}

TEST_F(ThemeExportTest, NativePickerCancellationDoesNotCreateJobAndProtectsAcrossInstances) {
    ThemeStore store(root, "https://unused.invalid");
    ThemeStore other(root / ".", "https://unused.invalid");
    const auto installed = store.install_local(definition, png, png);
    const auto package = acecode::path_from_utf8(installed.at("package_path"));
    fs::remove(package);
    const auto cancelled = store.start_export("ai-example", [&](const std::string& filename) -> std::optional<fs::path> {
        EXPECT_EQ(acecode::path_from_utf8(filename).extension(), ".zip");
        EXPECT_THROW(other.start_export("ai-example"), ThemeError);
        EXPECT_THROW(other.remove_local("ai-example"), ThemeError);
        EXPECT_THROW(other.install_local(definition, png, png), ThemeError);
        return std::nullopt;
    });
    EXPECT_EQ(cancelled["state"], "cancelled");
    EXPECT_FALSE(cancelled.contains("job_id"));
    EXPECT_FALSE(fs::exists(package));
    EXPECT_TRUE(other.remove_local("ai-example")["deleted"]);
}

TEST_F(ThemeExportTest, NativeSaveWritesCompleteZipAndInvalidDestinationPreservesExistingFile) {
    ThemeStore store(root / "themes", "https://unused.invalid");
    store.install_local(definition, png, png);
    const auto saved = root / "saved.zip";
    std::ofstream(saved) << "existing file";
    auto completed = finish(store, store.start_export("ai-example", [&](const std::string&) { return saved; }));
    ASSERT_EQ(completed["state"], "completed") << completed;
    EXPECT_TRUE(completed["native_saved"]);
    EXPECT_EQ(read(saved), store.export_download(completed["job_id"]));
    EXPECT_THROW(store.start_export("ai-example", [&](const std::string&) { return root / "missing/file.zip"; }), ThemeError);
    EXPECT_EQ(read(saved), store.export_download(completed["job_id"]));
    EXPECT_TRUE(store.remove_local("ai-example")["deleted"]);
    EXPECT_TRUE(fs::exists(saved));
}

TEST_F(ThemeExportTest, ZipProgressComesFromCompressionAndCancellationInterruptsIt) {
    ThemePackageFiles files{{"theme.json", definition.dump()}, {"background.png", std::string(500000, 'x')}, {"thumbnail.png", png}};
    std::vector<double> progress;
    write_theme_archive(root / "complete.zip", files, [&](double value) { progress.push_back(value); });
    ASSERT_GE(progress.size(), 2u);
    EXPECT_EQ(progress.front(), 0.0);
    EXPECT_EQ(progress.back(), 1.0);
    EXPECT_TRUE(std::is_sorted(progress.begin(), progress.end()));
    bool cancelled = false;
    try {
        write_theme_archive(root / "cancelled.zip", files,
            [&](double) { cancelled = true; }, [&] { return cancelled; });
        FAIL() << "Compression should have been cancelled";
    } catch (const ThemeError& error) { EXPECT_EQ(error.code, "THEME_CANCELLED"); }
}

TEST_F(ThemeExportTest, DeletionRollsBackBeforeFailureAndPreservesOtherThemesAndDrafts) {
    ThemeStore store(root, "https://unused.invalid");
    const auto one = store.install_local(definition, png, png);
    const auto other = theme_test::definition(png, "ai-example-2");
    const auto two = store.install_local(other, png, png);
    fs::create_directories(root / "drafts/source");
    std::ofstream(root / "drafts/source/background.png") << png;
    EXPECT_THROW(store.remove_local("ai-example", [] { throw ThemeError(500, "PERSIST_FAILED", "test"); }), ThemeError);
    EXPECT_EQ(store.definition("ai-example"), definition);
    EXPECT_TRUE(fs::exists(acecode::path_from_utf8(one.at("package_path"))));
    EXPECT_TRUE(store.remove_local("ai-example")["deleted"]);
    EXPECT_FALSE(fs::exists(root / "ai-example"));
    EXPECT_FALSE(fs::exists(acecode::path_from_utf8(one.at("package_path"))));
    EXPECT_TRUE(fs::exists(acecode::path_from_utf8(two.at("package_path"))));
    EXPECT_EQ(store.definition("ai-example-2"), other);
    EXPECT_TRUE(fs::exists(root / "drafts/source/background.png"));
}

TEST_F(ThemeExportTest, RejectsBuiltinsUnknownJobsAndLinkedThemeResources) {
    ThemeStore store(root, "https://unused.invalid");
    for (const auto* id : {"blue", "orange", "eva-01", "../outside", "ai-a/../../outside"}) {
        EXPECT_THROW(store.start_export(id), ThemeError);
        EXPECT_THROW(store.remove_local(id), ThemeError);
    }
    EXPECT_THROW(store.export_job("missing"), ThemeError);
    EXPECT_THROW(store.export_download("missing"), ThemeError);
    EXPECT_THROW(store.cancel_export("missing"), ThemeError);
    store.install_local(definition, png, png);
    fs::rename(root / "ai-example", root / "outside");
    std::error_code ec;
    fs::create_directory_symlink(root / "outside", root / "ai-example", ec);
    if (ec) GTEST_SKIP() << "Platform cannot create symlink: " << ec.message();
    EXPECT_THROW(store.start_export("ai-example"), ThemeError);
    EXPECT_THROW(store.remove_local("ai-example"), ThemeError);
    EXPECT_TRUE(fs::exists(root / "outside/1.0.0/background.png"));
}
}
