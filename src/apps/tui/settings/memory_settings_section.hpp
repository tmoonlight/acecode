#pragma once

#include "config/config.hpp"

#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>

#include <functional>
#include <memory>

namespace acecode::tui::settings {

// 设置中心「个性化」页的记忆区(openspec unify-memory-system 7.4):使用记忆、记忆摘要
// 开关与摘要模型(「当前模型」+ 已保存的模型)。每次改动立即经 set_memory_settings
// 写入 config.json —— 与网页 /api/config/memory 同一份配置 —— 再通过 published
// 回调让 TUI 把新值推给本进程的记忆运行时。条目浏览与删除在 TUI 里用 /memory。
class MemorySettingsSection {
public:
    MemorySettingsSection(AppConfig* config, std::function<void()> published);
    ~MemorySettingsSection();
    MemorySettingsSection(const MemorySettingsSection&) = delete;
    MemorySettingsSection& operator=(const MemorySettingsSection&) = delete;

    // 可聚焦的控件容器(挂进页面的 Container 里)。
    ftxui::Component component() const;
    // 当前状态的渲染(标题、说明、控件、保存结果)。
    ftxui::Element render() const;
    // 设置中心打开时按 config 刷新控件。
    void sync_from_config();

    struct State;

private:
    std::shared_ptr<State> state_;
};

} // namespace acecode::tui::settings
