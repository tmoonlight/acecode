#pragma once

#include "utils/utf8_path.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
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

// 给 target 目录建一个别名目录:Windows 用 junction(mklink /J,普通用户即可,本机
// C:\Users\shao → N:\Users\shao 就是这种形态),POSIX 用符号链接。失败返回 false,
// 调用方 GTEST_SKIP。
inline bool make_directory_alias(const std::filesystem::path& target,
                                 const std::filesystem::path& alias) {
#ifdef _WIN32
    const std::string cmd = "cmd /d /c mklink /J \"" + acecode::path_to_utf8(alias) + "\" \"" +
                            acecode::path_to_utf8(target) + "\" >NUL 2>&1";
    return std::system(cmd.c_str()) == 0 && std::filesystem::exists(alias);
#else
    std::error_code ec;
    std::filesystem::create_directory_symlink(target, alias, ec);
    return !ec;
#endif
}
}
