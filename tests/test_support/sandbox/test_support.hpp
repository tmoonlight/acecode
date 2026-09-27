#pragma once

#include "utils/utf8_path.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace acecode::sandbox::test {
// 每例使用独立的系统临时目录,清理只针对本例创建的树。
struct TempTree {
    std::filesystem::path root;
    TempTree() {
        static std::atomic<unsigned> sequence{0};
        root = std::filesystem::temp_directory_path() /
            ("acecode-sandbox-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence++));
        std::filesystem::create_directories(root);
        root = std::filesystem::canonical(root);
    }
    ~TempTree() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    std::filesystem::path dir(const std::string& name) {
        const auto path = root / name;
        std::filesystem::create_directories(path);
        return std::filesystem::canonical(path);
    }
    void write(const std::filesystem::path& path, const std::string& text = "original") {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }
};
}
