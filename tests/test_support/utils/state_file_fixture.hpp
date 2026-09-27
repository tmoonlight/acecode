#pragma once

#include <gtest/gtest.h>
#include "utils/state_file.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace acecode::test_support {
namespace fs = std::filesystem;

class StateFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 在临时目录里给本测试一份独立 state.json 路径。
        auto tmp = fs::temp_directory_path() /
                   ("acecode_state_test_" + std::to_string(std::rand()));
        fs::create_directories(tmp);
        path_ = (tmp / "state.json").string();
        acecode::set_state_file_path_for_test(path_);
    }
    void TearDown() override {
        acecode::set_state_file_writes_paused(false);
        acecode::set_state_file_path_for_test("");
        std::error_code ec;
        fs::remove_all(fs::path(path_).parent_path(), ec);
    }
    std::string path_;
};

inline void write_raw(const std::string& path, const std::string& contents) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs << contents;
}

} // namespace acecode::test_support
