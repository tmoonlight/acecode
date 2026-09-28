#include "retry_progress.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <utility>

namespace acecode::agent::detail {

// 人类可读的字节量:< 1KB 显示原始字节,否则进位到 KB / MB(保留一位小数)。
// 进度文案里直接打印原始字节数(如 "8641 字节")观感上会显得异常地大,统一走这里。
std::string human_bytes(std::size_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " 字节";
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(1);
    double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0) oss << kb << " KB";
    else oss << (kb / 1024.0) << " MB";
    return oss.str();
}

std::string format_bytes_detail(std::size_t bytes) {
    return "参数 " + human_bytes(bytes);
}

} // namespace acecode::agent::detail
