#pragma once

#include "encoding.hpp"

#include <filesystem>
#include <string>
#include <system_error>

namespace acecode {

inline std::filesystem::path path_from_utf8(const std::string& text) {
#ifdef _WIN32
    return std::filesystem::path(utf8_to_wide(text));
#else
    return std::filesystem::path(text);
#endif
}

inline std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    return wide_to_utf8(path.wstring());
#else
    return path.string();
#endif
}

inline std::string path_to_utf8_generic(const std::filesystem::path& path) {
#ifdef _WIN32
    return wide_to_utf8(path.generic_wstring());
#else
    return path.generic_string();
#endif
}

// 把绝对路径转成 Windows 扩展长度形式(`\\?\C:\...` / `\\?\UNC\srv\share\...`),
// 让 std::filesystem 的 IO 调用越过 MAX_PATH(260)限制。
//
// 只用于文件 IO(枚举 / stat / 建目录 / copy / rename / remove_all):返回值不得写进
// JSON、指针文件或日志,也不能拿来和普通形态的路径做比较 —— 对外一律用原始形态。
//
// 规则:先 lexically_normal(统一分隔符、消掉 `.` / `..`;`\\?\` 会跳过 Win32 的规范化,
// 所以必须先做);已带 `\\?\` / `\\.\` / `\??\` 前缀的原样返回;相对路径(含 `\x`
// 这种根相对与 `C:x` 这种盘符相对)原样返回。POSIX 上为恒等函数。
inline std::filesystem::path to_extended_length_path(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring& raw = path.native();
    auto has_prefix = [](const std::wstring& text, const wchar_t* prefix) {
        return text.rfind(prefix, 0) == 0;
    };
    if (has_prefix(raw, L"\\\\?\\") || has_prefix(raw, L"\\\\.\\") || has_prefix(raw, L"\\??\\")) {
        return path;
    }
    if (!path.is_absolute()) return path;
    const std::wstring normal = path.lexically_normal().native();
    if (normal.size() >= 2 && normal[0] == L'\\' && normal[1] == L'\\') {
        return std::filesystem::path(L"\\\\?\\UNC\\" + normal.substr(2));
    }
    return std::filesystem::path(L"\\\\?\\" + normal);
#else
    return path;
#endif
}

inline std::string current_path_utf8() {
    std::error_code ec;
    auto path = std::filesystem::current_path(ec);
    if (ec) return {};
    return path_to_utf8(path);
}

} // namespace acecode
