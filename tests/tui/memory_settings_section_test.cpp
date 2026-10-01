// 覆盖 src/apps/tui/settings/memory_settings_section.{hpp,cpp}(openspec unify-memory-system 7.4):
// TUI 设置中心「个性化」页的记忆区 —— 渲染使用记忆 / 记忆摘要 / 摘要模型三项控件;
// 在 TUI 里打开记忆摘要后,config.json 与网页 GET /api/config/memory 看到的是同一份值。

#include "tui/settings/memory_settings_section.hpp"

#include "config/config.hpp"
#include "test_support/memory/memory_test_home.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"
#include "web/handlers/memory_handler.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

namespace {

acecode::AppConfig config_with_models() {
    acecode::AppConfig config;
    acecode::ModelProfile fast;
    fast.name = "fast";
    fast.provider = "copilot";
    fast.model = "gpt-4o";
    config.saved_models.push_back(fast);
    return config;
}

std::string render(const acecode::tui::settings::MemorySettingsSection& section) {
    ftxui::Screen screen(120, 30);
    ftxui::Render(screen, section.render());
    return screen.ToString();
}

} // namespace

// 场景:打开个性化页。期望:记忆区列出三项控件,摘要模型选项含「当前模型」与已保存模型。
TEST(TuiMemorySettingsSection, RendersControlsAndModelChoices) {
    acecode_test::MemoryTestHome home("tui-memory-settings-render");
    auto config = config_with_models();
    acecode::tui::settings::MemorySettingsSection section(&config, {});
    const std::string text = render(section);
    EXPECT_NE(text.find("Use memory"), std::string::npos) << text;
    EXPECT_NE(text.find("Memory summarization"), std::string::npos) << text;
    EXPECT_NE(text.find("Summary model"), std::string::npos) << text;
    EXPECT_NE(text.find("Current model"), std::string::npos) << text;
    EXPECT_NE(text.find("fast"), std::string::npos) << text;
}

// 场景:在 TUI 里把焦点移到「记忆摘要」并按空格打开。
// 期望:立即写入 config.json、运行中配置随之更新、发布回调被调用;从磁盘重读后按网页
// GET /api/config/memory 的口径序列化,与 TUI 内存里的值一致。
TEST(TuiMemorySettingsSection, TogglingSummaryPersistsSameValueTheWebReads) {
    acecode_test::MemoryTestHome home("tui-memory-settings-toggle");
    auto config = config_with_models();
    int published = 0;
    acecode::tui::settings::MemorySettingsSection section(&config, [&published] { ++published; });
    auto component = section.component();
    component->OnEvent(ftxui::Event::ArrowDown);
    ASSERT_TRUE(component->OnEvent(ftxui::Event::Character(' ')));

    EXPECT_TRUE(config.memory.summary.enabled);
    EXPECT_EQ(published, 1);
    const std::string config_path =
        acecode::path_to_utf8(acecode::path_from_utf8(acecode::get_acecode_dir()) / "config.json");
    const auto loaded = acecode::load_config_from_path(config_path);
    EXPECT_TRUE(loaded.memory.summary.enabled);
    EXPECT_EQ(acecode::web::memory_settings_json(loaded.memory, true),
              acecode::web::memory_settings_json(config.memory, true));
    EXPECT_NE(render(section).find("Memory settings saved."), std::string::npos);
}
