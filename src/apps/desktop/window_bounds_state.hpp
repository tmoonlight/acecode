#pragma once

#include "window_size.hpp"

#include <nlohmann/json.hpp>

#include <optional>

namespace acecode::desktop {

// state.json 的 desktop_window 段:上次退出时的主窗口位置和尺寸。
// 字段缺失 / 类型不符 / 矩形退化 / DPI 不合理 → nullopt,永不抛异常。
std::optional<SavedDesktopWindowBounds> parse_desktop_window_bounds(
    const nlohmann::json& value);
nlohmann::json desktop_window_bounds_json(const SavedDesktopWindowBounds& bounds);

std::optional<SavedDesktopWindowBounds> read_desktop_window_bounds();
// 原子写,保留其他 key;失败只记日志不阻断退出。
void write_desktop_window_bounds(const SavedDesktopWindowBounds& bounds);

} // namespace acecode::desktop
