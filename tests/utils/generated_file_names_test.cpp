#include <gtest/gtest.h>

#include "utils/generated_file_names.hpp"
#include "utils/uuid.hpp"

#include <filesystem>
#include <fstream>

namespace {

TEST(GeneratedFileNames, NewPathsUseUnderscoresWithoutChangingParent) {
    const auto root = std::filesystem::temp_directory_path() / "acecode_naming_parent-with-hyphen";
    EXPECT_EQ(acecode::generated_file_name("upgrade-2026-10-16-123.log"),
              "upgrade_2026_10_16_123.log");
    EXPECT_EQ(acecode::compatible_generated_path(root, "new-data.json"),
              root / "new_data.json");
}

TEST(GeneratedFileNames, LegacyStoreWinsWithoutCreatingAnEmptyReplacement) {
    const auto root = std::filesystem::temp_directory_path() /
        ("acecode_naming_" + acecode::generated_file_name(acecode::generate_uuid()));
    std::filesystem::create_directories(root);
    const auto old_path = root / "scheduled-loops.sqlite3";
    const auto new_path = root / "scheduled_loops.sqlite3";
    { std::ofstream out(old_path); out << "existing data"; }
    EXPECT_EQ(acecode::compatible_generated_path(root, "scheduled-loops.sqlite3"), old_path);
    EXPECT_FALSE(std::filesystem::exists(new_path));
    { std::ofstream out(new_path); out << "separate data"; }
    EXPECT_EQ(acecode::compatible_generated_path(root, "scheduled-loops.sqlite3"), old_path);
    std::filesystem::remove(old_path);
    EXPECT_EQ(acecode::compatible_generated_path(root, "scheduled-loops.sqlite3"), new_path);
    std::filesystem::remove(new_path);
    std::filesystem::remove(root);
}

} // namespace
