#pragma once

// 权限模式的词汇表(P2-05 自 permissions.hpp 拆出):config 只需要解析 / 序列化模式名,
// 不该依赖整个 PermissionManager。config/vocab 只能 include 标准库(R6)。

#include <optional>
#include <string>

namespace acecode {

// Permission mode for the current session
enum class PermissionMode {
    Default,      // Prompt for write/exec tools, auto-allow read-only
    // Auto(openspec add-auto-mode-sandbox,复刻 Codex 的 Auto 预设,取代原
    // accept-edits):文件编辑自动放行;bash 走 src/sandbox 的决策表 ——
    // 已知安全命令与沙盒内的未知命令直接跑,危险命令 / 越权申请 / 无沙盒时
    // 的未知命令才确认。老名字 accept-edits / acceptEdits 仍可解析。
    Auto,
    Yolo,         // Auto-allow all tool permissions without prompting (no sandbox)
    Plan,         // Explore and write only the active plan file before approval
    // AutoReview(openspec add-auto-review-mode,界面名「自动模式（试验）」):放行表与
    // Auto 完全相同;差别只在「要确认」的调用先交给裁决模型,而不是直接弹窗。
    // 追加在枚举末尾,不改变已有取值。
    AutoReview
};

inline const char* permission_mode_name(PermissionMode m) {
    switch (m) {
        case PermissionMode::Default:    return "default";
        case PermissionMode::Auto:       return "auto";
        case PermissionMode::Yolo:       return "yolo";
        case PermissionMode::Plan:       return "plan";
        case PermissionMode::AutoReview: return "auto-review";
    }
    return "unknown";
}

// AutoReview 沿用 Auto 的放行表(exec 决策表、文件工具自动放行、headless 的 MCP
// 白名单)。凡是「按 Auto 处理」的判断都经这一个函数,不要各处手写两个枚举值。
inline bool permission_mode_uses_auto_policy(PermissionMode m) {
    return m == PermissionMode::Auto || m == PermissionMode::AutoReview;
}

// 模式名解析的唯一入口:别名(accept-edits / acceptEdits = auto)只在这里
// 维护。返回 nullopt = 未知名字,调用方自己决定报错还是回退。
inline std::optional<PermissionMode> parse_permission_mode_name(std::string name) {
    // 去两端空白。
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t' ||
                             name.back() == '\r' || name.back() == '\n')) {
        name.pop_back();
    }
    std::size_t start = 0;
    while (start < name.size() && (name[start] == ' ' || name[start] == '\t')) ++start;
    name = name.substr(start);
    if (name == "default") return PermissionMode::Default;
    if (name == "auto" || name == "accept-edits" || name == "acceptEdits") {
        return PermissionMode::Auto;
    }
    if (name == "yolo") return PermissionMode::Yolo;
    if (name == "plan") return PermissionMode::Plan;
    if (name == "auto-review") return PermissionMode::AutoReview;
    return std::nullopt;
}

// 解析后再序列化:把别名归一成线上名("accept-edits" → "auto"),未知名原样返回。
inline std::string canonical_permission_mode_name(const std::string& name) {
    if (auto mode = parse_permission_mode_name(name)) return permission_mode_name(*mode);
    return name;
}

} // namespace acecode
