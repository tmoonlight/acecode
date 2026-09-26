#include "image/image_processor.hpp"
#include "skills/default_skill_seeder.hpp"
#include "skills/skill_registry.hpp"
#include "test_support/repo_root.hpp"
#include "themes/theme_store.hpp"
#include "utils/sha256.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

std::string read_bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

class AiThemeSeedTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 用例加深目录后仍定位真实种子，避免把错误路径当成缺少资源。
        repository_ = acecode::test_support::find_repo_root(__FILE__);
        packaged_ = repository_ / "assets" / "seed";
        root_ = fs::temp_directory_path() /
            ("acecode-ai-theme-seed-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(fs::create_directory(root_));
        home_ = root_ / "profile" / ".acecode";
        fs::create_directories(home_);
    }

    void TearDown() override {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    void write(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        ASSERT_TRUE(stream.is_open());
        stream << text;
        ASSERT_TRUE(stream.good());
    }

    const acecode::DefaultSkillSeedOutcome* theme_outcome(
        const acecode::DefaultSkillSeedInstallResult& result) {
        const auto found = std::find_if(
            result.outcomes.begin(), result.outcomes.end(),
            [](const auto& outcome) { return outcome.name == "ai-theme"; });
        return found == result.outcomes.end() ? nullptr : &*found;
    }

    fs::path repository_;
    fs::path packaged_;
    fs::path root_;
    fs::path home_;
};

TEST_F(AiThemeSeedTest, PreviousUserVersionReceivesDiscoverableThemeAndResources) {
    write(home_ / "seed.version", "2026-09-09.1\n");

    const auto result = acecode::reconcile_default_global_skills(
        home_, packaged_ / "skills");
    ASSERT_TRUE(result.error.empty()) << result.error;
    ASSERT_TRUE(result.version_written);
    const auto* outcome = theme_outcome(result);
    ASSERT_NE(outcome, nullptr);
    EXPECT_EQ(outcome->result, "installed");
    EXPECT_EQ(outcome->relative_path, "acecode/ai-theme");
    EXPECT_TRUE(outcome->acecode_owned);
    EXPECT_EQ(outcome->source_tree_sha256, outcome->installed_tree_sha256);

    acecode::SkillRegistry registry;
    registry.set_scan_roots({home_ / "skills"});
    registry.scan();
    const auto skill = registry.find("ai-theme");
    ASSERT_TRUE(skill.has_value());
    EXPECT_EQ(skill->command_key, "ai-theme");
    EXPECT_EQ(skill->category, "acecode");
    EXPECT_FALSE(skill->description.empty());
    EXPECT_NE(std::find(skill->tags.begin(), skill->tags.end(), "themes"),
              skill->tags.end());

    const auto files = registry.list_supporting_files("ai-theme");
    const std::set<std::string> expected = {
        "assets/acecode-home-reference.jpg",
        "assets/acecode-home-light.png",
        "assets/acecode-home-dark.png",
        "assets/preview-light.html",
        "assets/preview-dark.html",
        "scripts/render_preview.py",
        "scripts/artboard.js",
        "scripts/preview.js",
        "scripts/preview.css",
        "references/customization-questions.md",
        "references/browser-preview.md",
        "references/image-prompts.md",
        "references/palette-example.json",
        "references/theme-contract.md",
    };
    EXPECT_EQ(std::set<std::string>(files.begin(), files.end()), expected);
    for (const auto& relative : expected) {
        const auto installed = registry.resolve_skill_file("ai-theme", relative);
        ASSERT_TRUE(installed.has_value()) << relative;
        const fs::path original = packaged_ / "skills" / "acecode" /
            "ai-theme" / relative;
        EXPECT_EQ(acecode::sha256_hex(read_bytes(*installed)),
                  acecode::sha256_hex(read_bytes(original))) << relative;
    }

    const auto image = registry.resolve_skill_file(
        "ai-theme", "assets/acecode-home-reference.jpg");
    ASSERT_TRUE(image.has_value());
    std::string image_error;
    const auto info = acecode::image::probe_image_info(
        read_bytes(*image), &image_error);
    ASSERT_TRUE(info.has_value()) << image_error;
    EXPECT_GE(info->width, 1000);
    EXPECT_GE(info->height, 600);
    EXPECT_GT(info->width, info->height);

    const auto retry = acecode::reconcile_default_global_skills(
        home_, packaged_ / "skills");
    EXPECT_TRUE(retry.error.empty()) << retry.error;
    EXPECT_FALSE(retry.version_written);
    EXPECT_EQ(read_bytes(home_ / "seed.version"),
              read_bytes(packaged_ / "seed.version"));
}

TEST_F(AiThemeSeedTest, PaletteExampleMatchesThemeColorContract) {
    const fs::path example = packaged_ / "skills" / "acecode" /
        "ai-theme" / "references" / "palette-example.json";
    const auto palette = nlohmann::json::parse(read_bytes(example));
    ASSERT_TRUE(palette["name"].is_string());
    EXPECT_FALSE(palette["name"].get<std::string>().empty());
    const std::string mode = palette.value("mode", "");
    EXPECT_TRUE(mode == "light" || mode == "dark");
    ASSERT_TRUE(palette["colors"].is_object());
    ASSERT_EQ(palette["colors"].size(), 28u);

    const auto existing = nlohmann::json::parse(read_bytes(
        repository_ / "assets" / "themes" / "eva-01" / "palette.json"));
    std::set<std::string> expected_keys;
    for (const auto& color : existing.items()) expected_keys.insert(color.key());
    std::set<std::string> actual_keys;
    const std::regex hex_color("^#[0-9A-Fa-f]{6}$");
    for (const auto& color : palette["colors"].items()) {
        actual_keys.insert(color.key());
        ASSERT_TRUE(color.value().is_string()) << color.key();
        EXPECT_TRUE(std::regex_match(
            color.value().get<std::string>(), hex_color)) << color.key();
    }
    EXPECT_EQ(actual_keys, expected_keys);
    ASSERT_TRUE(palette.contains("appearance"));
    EXPECT_TRUE(acecode::themes::valid_theme_appearance(palette.at("appearance")));
    EXPECT_EQ(palette.at("appearance").size(), 3u);
    EXPECT_TRUE(palette.at("appearance").contains("logo_color"));
    EXPECT_TRUE(palette.at("appearance").contains("home_title_color"));
    EXPECT_TRUE(palette.at("appearance").contains("extend_to_titlebar"));
}

TEST_F(AiThemeSeedTest, SurfaceRevisionUpdatesPreviouslyManagedThemeSkillAndReferences) {
    const fs::path previous = root_ / "previous-seed";
    fs::copy(packaged_, previous, fs::copy_options::recursive);
    write(previous / "seed.version", "2026-09-12.1\n");
    const fs::path relative = fs::path("skills") / "acecode" / "ai-theme";
    fs::remove(previous / relative / "scripts" / "artboard.js");
    const auto previous_skill = read_bytes(previous / relative / "SKILL.md") + "\nPrevious revision.\n";
    write(previous / relative / "SKILL.md", previous_skill);
    auto previous_palette = nlohmann::json::parse(read_bytes(
        previous / relative / "references" / "palette-example.json"));
    previous_palette.erase("appearance");
    write(previous / relative / "references" / "palette-example.json", previous_palette.dump(2));
    const auto initial = acecode::reconcile_default_global_skills(home_, previous / "skills");
    ASSERT_TRUE(initial.error.empty()) << initial.error;
    ASSERT_TRUE(initial.version_written);
    ASSERT_EQ(read_bytes(home_ / relative / "SKILL.md"), previous_skill);
    ASSERT_FALSE(fs::exists(home_ / relative / "scripts" / "artboard.js"));

    // An installed official copy keeps its ownership when the source ID advances.
    auto previous_state = nlohmann::json::parse(read_bytes(initial.state_path));
    for (auto& entry : previous_state.at("skills"))
        if (entry.at("name") == "ai-theme")
            entry["source_id"] = "acecode:ai-theme@2026-09-12";
    write(initial.state_path, previous_state.dump(2));

    const auto updated = acecode::reconcile_default_global_skills(home_, packaged_ / "skills");
    ASSERT_TRUE(updated.error.empty()) << updated.error;
    ASSERT_TRUE(updated.version_written);
    const auto* outcome = theme_outcome(updated);
    ASSERT_NE(outcome, nullptr);
    EXPECT_EQ(outcome->result, "updated");
    EXPECT_NE(outcome->source_id, "acecode:ai-theme@2026-09-12");
    EXPECT_TRUE(outcome->acecode_owned);
    EXPECT_EQ(outcome->source_tree_sha256, outcome->installed_tree_sha256);
    EXPECT_EQ(read_bytes(home_ / relative / "SKILL.md"), read_bytes(packaged_ / relative / "SKILL.md"));
    EXPECT_EQ(read_bytes(home_ / relative / "scripts" / "artboard.js"),
              read_bytes(packaged_ / relative / "scripts" / "artboard.js"));
    EXPECT_EQ(read_bytes(home_ / relative / "references" / "palette-example.json"),
              read_bytes(packaged_ / relative / "references" / "palette-example.json"));
    EXPECT_EQ(read_bytes(home_ / "seed.version"), read_bytes(packaged_ / "seed.version"));
}

TEST_F(AiThemeSeedTest, MessageScaleRevisionUpdatesPreviousManagedThemeAndPreview) {
    const fs::path previous = root_ / "previous-seed";
    fs::copy(packaged_, previous, fs::copy_options::recursive);
    write(previous / "seed.version", "2026-09-15.2\n");
    const fs::path relative = fs::path("skills") / "acecode" / "ai-theme";
    const std::string previous_skill =
        "---\nname: ai-theme\ndescription: Previous official theme workflow\n"
        "metadata:\n  source_id: acecode:ai-theme@2026-09-15.2\n---\n"
        "Scale user-message artwork to each bubble width, centered at the bottom.\n";
    write(previous / relative / "SKILL.md", previous_skill);
    const std::vector<fs::path> previews = {
        fs::path("scripts") / "preview.js",
        fs::path("assets") / "preview-light.html",
        fs::path("assets") / "preview-dark.html",
    };
    for (const auto& preview : previews) {
        auto bytes = read_bytes(previous / relative / preview);
        const auto size_at = bytes.find("720px auto");
        ASSERT_NE(size_at, std::string::npos) << preview;
        bytes.replace(size_at, std::string("720px auto").size(), "100% auto");
        const auto position_at = bytes.find("right bottom");
        if (position_at != std::string::npos)
            bytes.replace(position_at, std::string("right bottom").size(), "center bottom");
        write(previous / relative / preview, bytes);
    }

    const auto initial = acecode::reconcile_default_global_skills(home_, previous / "skills");
    ASSERT_TRUE(initial.error.empty()) << initial.error;
    ASSERT_TRUE(initial.version_written);
    ASSERT_EQ(read_bytes(home_ / "seed.version"), "2026-09-15.2\n");
    auto previous_state = nlohmann::json::parse(read_bytes(initial.state_path));
    for (auto& entry : previous_state.at("skills"))
        if (entry.at("name") == "ai-theme")
            entry["source_id"] = "acecode:ai-theme@2026-09-15.2";
    write(initial.state_path, previous_state.dump(2));

    const auto updated = acecode::reconcile_default_global_skills(home_, packaged_ / "skills");
    ASSERT_TRUE(updated.error.empty()) << updated.error;
    ASSERT_TRUE(updated.version_written);
    const auto* outcome = theme_outcome(updated);
    ASSERT_NE(outcome, nullptr);
    EXPECT_EQ(outcome->result, "updated");
    EXPECT_EQ(outcome->source_id, "acecode:ai-theme@2026-09-15.3");
    EXPECT_TRUE(outcome->acecode_owned);
    EXPECT_EQ(outcome->source_tree_sha256, outcome->installed_tree_sha256);
    EXPECT_EQ(read_bytes(home_ / "seed.version"), read_bytes(packaged_ / "seed.version"));
    EXPECT_EQ(read_bytes(home_ / relative / "SKILL.md"), read_bytes(packaged_ / relative / "SKILL.md"));
    for (const auto& preview : previews) {
        const auto installed = read_bytes(home_ / relative / preview);
        EXPECT_EQ(installed, read_bytes(packaged_ / relative / preview));
        EXPECT_NE(installed.find("720px auto"), std::string::npos) << preview;
        if (preview.extension() == ".html")
            EXPECT_NE(installed.find("background-position: right bottom"), std::string::npos) << preview;
    }
    EXPECT_EQ(read_bytes(home_ / relative / "scripts" / "artboard.js"),
              read_bytes(packaged_ / relative / "scripts" / "artboard.js"));
}

TEST_F(AiThemeSeedTest, UpgradePreservesUserAuthoredThemeSkill) {
    const fs::path skill = home_ / "skills" / "acecode" / "ai-theme";
    const std::string user_skill =
        "---\nname: ai-theme\ndescription: User theme workflow\n---\n"
        "Use my own illustration references.\n";
    write(skill / "SKILL.md", user_skill);
    write(skill / "references" / "personal.md", "My reference notes.\n");
    write(home_ / "seed.version", "2026-09-09.1\n");

    const auto result = acecode::reconcile_default_global_skills(
        home_, packaged_ / "skills");
    ASSERT_TRUE(result.error.empty()) << result.error;
    ASSERT_TRUE(result.version_written);
    const auto* outcome = theme_outcome(result);
    ASSERT_NE(outcome, nullptr);
    EXPECT_FALSE(outcome->acecode_owned);
    EXPECT_EQ(read_bytes(skill / "SKILL.md"), user_skill);
    EXPECT_EQ(read_bytes(skill / "references" / "personal.md"),
              "My reference notes.\n");
    EXPECT_FALSE(fs::exists(skill / "assets" / "acecode-home-reference.jpg"));
}

} // namespace
