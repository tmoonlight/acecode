#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace acecode::utils {

// 保留既有 std::isspace/std::tolower 的 unsigned-char 语义,不改为空白四字符集合。
inline std::string trim_ascii_copy(const std::string& raw) {
    std::size_t first = 0;
    while (first < raw.size() &&
           std::isspace(static_cast<unsigned char>(raw[first])) != 0) {
        ++first;
    }
    std::size_t last = raw.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(raw[last - 1])) != 0) {
        --last;
    }
    return raw.substr(first, last - first);
}

inline std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace acecode::utils
