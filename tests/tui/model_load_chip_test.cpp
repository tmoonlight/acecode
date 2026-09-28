// P0-09 回归:底栏「模型负载」chip 与 model-pool 轮询回调必须共用同一个原子变量。
//
// 触发场景:main.cpp 曾经自带一份 file-static 的 g_model_load_percent 与
// render_model_load_chip 孪生实现,轮询回调写的是 main.cpp 那份原子,而
// tui_helpers.cpp 这份公共实现读的是自己的原子。两份一旦分家,写入方与渲染方
// 各看各的,谁也看不到对方的值。
// 期望行为:通过 acecode::tui::g_model_load_percent 写入的百分比,公共的
// render_model_load_chip() 必须原样渲染出来;写回 -1(未知)时 chip 为空。
// 回归时的表现:负载 chip 永不显示,或者永远停在旧值,底栏看不到「▁▃▅▇ NN%」。
#include <gtest/gtest.h>

#include "tui/render/status_chips.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <string>

namespace {

// g_model_load_percent 是进程级状态,用 RAII 守卫恢复,避免污染同进程的其它用例。
struct ScopedModelLoadPercent {
    int previous;
    explicit ScopedModelLoadPercent(int value)
        : previous(acecode::tui::g_model_load_percent.load()) {
        acecode::tui::g_model_load_percent.store(value);
    }
    ~ScopedModelLoadPercent() {
        acecode::tui::g_model_load_percent.store(previous);
    }
};

// 只取每个格子的字符,不带颜色转义,便于断言文本内容。
std::string render_plain_text(const ftxui::Element& element) {
    ftxui::Screen screen(24, 1);
    ftxui::Render(screen, element);
    std::string out;
    for (int x = 0; x < screen.dimx(); ++x) {
        out += screen.PixelAt(x, 0).character;
    }
    return out;
}

}  // namespace

TEST(ModelLoadChip, RendersPercentWrittenThroughSharedAtomic) {
    ScopedModelLoadPercent guard(42);
    const std::string text =
        render_plain_text(acecode::tui::render_model_load_chip());
    EXPECT_NE(text.find("42%"), std::string::npos) << "rendered: [" << text << "]";
}

TEST(ModelLoadChip, PercentUpdatesAreVisibleOnNextRender) {
    ScopedModelLoadPercent guard(10);
    EXPECT_NE(render_plain_text(acecode::tui::render_model_load_chip()).find("10%"),
              std::string::npos);
    acecode::tui::g_model_load_percent.store(95);
    const std::string text =
        render_plain_text(acecode::tui::render_model_load_chip());
    EXPECT_NE(text.find("95%"), std::string::npos) << "rendered: [" << text << "]";
    EXPECT_EQ(text.find("10%"), std::string::npos) << "rendered: [" << text << "]";
}

TEST(ModelLoadChip, UnknownPercentRendersNothing) {
    ScopedModelLoadPercent guard(-1);
    const std::string text =
        render_plain_text(acecode::tui::render_model_load_chip());
    EXPECT_EQ(text.find_first_not_of(' '), std::string::npos)
        << "rendered: [" << text << "]";
}
