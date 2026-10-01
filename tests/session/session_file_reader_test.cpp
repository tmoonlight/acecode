#include "session/session_file_reader.hpp"
#include "utils/atomic_file.hpp"
#include "utils/uuid.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

TEST(SessionFileReader, AppendPreservesBoundAndAtomicReplacementInvalidatesIdentity) {
    const auto root = std::filesystem::temp_directory_path() / ("ace-reader-" + acecode::generate_uuid());
    std::filesystem::create_directories(root);
    const auto path = root / "history.jsonl";
    std::ofstream(path, std::ios::binary) << "original\n";
    {
        acecode::SessionFileReader reader(path.string());
        ASSERT_TRUE(reader.valid());
        const auto prefix = reader.prefix();
        std::ofstream(path, std::ios::binary | std::ios::app) << "appended\n";
        EXPECT_TRUE(reader.unchanged(prefix));
        EXPECT_EQ(reader.read(0, 1000), "original\n");
        EXPECT_TRUE(acecode::atomic_write_file(path.string(), "original\nrewritten\n", false, true));
        EXPECT_FALSE(reader.unchanged(prefix)); // Identical prefix is insufficient after replace.
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
