// 覆盖 src/apps/installer/installer_timeline:安装界面在启动浮窗动画上延伸出的几个场景。
//   1. 欢迎页与启动浮窗开场逐帧一致;内容在开场播完后才淡入。
//   2. 安装中水位随进度单调上涨,进度 1 时水面越过图标原位很多。
//   3. 完成场景:水退回原位并定格成图标波浪,'>' 在定格时再亮一次,之后完全安静。
//   4. 卸载中水位随进度退到方砖以下;已卸载场景结束时 'A' 与 '>' 都收起、方砖变淡缩小。
//   5. 显示进度只向目标靠拢,不越过目标也不倒退。

#include "installer/installer_timeline.hpp"

#include <gtest/gtest.h>

namespace timeline = acecode::installer::timeline;
namespace splash_anim = acecode::desktop::splash_anim;

// 场景:欢迎页第 0 / 700 / 3000 毫秒取帧。
// 期望:与 splash_anim::frame_at(t, 未就绪) 完全相同(安装窗口打开时的开场就是启动浮窗那一段);
// 开场进行到 520ms 前内容不显示,900ms 后完全显示。
TEST(InstallerTimeline, WelcomeReusesSplashIntro) {
    for (double t : {0.0, 700.0, 3000.0}) {
        const auto ours = timeline::welcome(t);
        const auto splash = splash_anim::frame_at(t, std::nullopt);
        EXPECT_DOUBLE_EQ(ours.tile_scale, splash.tile_scale) << t;
        EXPECT_DOUBLE_EQ(ours.chevron_draw, splash.chevron_draw) << t;
        EXPECT_DOUBLE_EQ(ours.wave_phase, splash.wave_phase) << t;
    }
    EXPECT_DOUBLE_EQ(timeline::content_reveal(400.0), 0.0);
    EXPECT_DOUBLE_EQ(timeline::content_reveal(timeline::kContentRevealEndMs), 1.0);
}

// 场景:安装进度 0 → 0.5 → 1。
// 期望:wave_rise 单调上涨,进度 0 时正好在图标原位(1.0),进度 1 时等于 1 + kFillTravel;
// 完整图标('A'、'>')一直在,不会因为用户在开场没播完时点了安装而缺笔画。
TEST(InstallerTimeline, InstallingRaisesWaterWithProgress) {
    const auto empty = timeline::installing(200.0, 0.0);
    const auto half = timeline::installing(200.0, 0.5);
    const auto full = timeline::installing(200.0, 1.0);
    EXPECT_DOUBLE_EQ(empty.wave_rise, 1.0);
    EXPECT_LT(empty.wave_rise, half.wave_rise);
    EXPECT_LT(half.wave_rise, full.wave_rise);
    EXPECT_DOUBLE_EQ(full.wave_rise, 1.0 + timeline::kFillTravel);
    EXPECT_DOUBLE_EQ(empty.a_reveal, 1.0);
    EXPECT_DOUBLE_EQ(empty.chevron_draw, 1.0);
    EXPECT_DOUBLE_EQ(empty.chevron_offset, 0.0);
}

// 场景:进度 1 时进入完成场景,分别看开始、定格时刻、定格后 2 秒。
// 期望:开始时水位还在高处;定格时 '>' 辉光为 1、方砖有白热;2 秒后水位回到原位(1.0)、
// 波浪定格(morph=1),辉光、白热、震动全部归零。
TEST(InstallerTimeline, FinishedSettlesAndCelebratesOnce) {
    const auto start = timeline::finished(0.0, 5000.0, 1.0);
    EXPECT_GT(start.wave_rise, 1.5);
    const auto celebrate = timeline::finished(timeline::kFinishCelebrateMs, 5000.0, 1.0);
    EXPECT_DOUBLE_EQ(celebrate.chevron_glow, 1.0);
    EXPECT_GT(celebrate.heat, 0.0);
    const auto later = timeline::finished(timeline::kFinishCelebrateMs + 2000.0, 7000.0, 1.0);
    EXPECT_DOUBLE_EQ(later.wave_rise, 1.0);
    EXPECT_DOUBLE_EQ(later.wave_morph, 1.0);
    EXPECT_DOUBLE_EQ(later.chevron_glow, 0.0);
    EXPECT_DOUBLE_EQ(later.heat, 0.0);
    EXPECT_DOUBLE_EQ(later.shake_x, 0.0);
    EXPECT_DOUBLE_EQ(later.shake_y, 0.0);
}

// 场景:失败场景开始与 1 秒后。
// 期望:开始时轻震(shake 非零),1 秒后静止在图标原位,不会一直抖。
TEST(InstallerTimeline, FailedShakesThenRests) {
    const auto start = timeline::failed(10.0, 5000.0, 0.6);
    EXPECT_TRUE(start.shake_x != 0.0 || start.shake_y != 0.0);
    const auto later = timeline::failed(1000.0, 6000.0, 0.6);
    EXPECT_DOUBLE_EQ(later.wave_rise, 1.0);
    EXPECT_DOUBLE_EQ(later.shake_x, 0.0);
    EXPECT_DOUBLE_EQ(later.wave_morph, 1.0);
}

// 场景:卸载进度 0 / 1,以及已卸载场景播完。
// 期望:卸载进度 1 时水完全退到方砖以下(wave_rise=0);已卸载播完后 'A' 擦掉、'>' 收回且飞到右侧,
// 方砖缩小并只剩约三分之一的不透明度。
TEST(InstallerTimeline, UninstallDrainsAndRemovedFades) {
    EXPECT_DOUBLE_EQ(timeline::uninstalling(3000.0, 0.0).wave_rise, 1.0);
    EXPECT_DOUBLE_EQ(timeline::uninstalling(3000.0, 1.0).wave_rise, 0.0);
    const auto gone = timeline::removed(timeline::kRemovedTotalMs + 100.0);
    EXPECT_DOUBLE_EQ(gone.a_reveal, 0.0);
    EXPECT_DOUBLE_EQ(gone.chevron_draw, 0.0);
    EXPECT_GT(gone.chevron_offset, 300.0);
    EXPECT_NEAR(gone.tile_opacity, 0.35, 1e-9);
    EXPECT_LT(gone.tile_scale, 0.7);
    const auto begin = timeline::removed(0.0);
    EXPECT_DOUBLE_EQ(begin.a_reveal, 1.0);
    EXPECT_DOUBLE_EQ(begin.tile_opacity, 1.0);
}

// 场景:目标进度 0.8,从 0 开始每 16ms 靠拢一次;之后目标回退到 0.5。
// 期望:显示进度单调增加、永远不超过 0.8;每帧最多涨 16/kMinFillMs(本机半秒就装完时
// 水位动画也要播满约 1.5 秒);约 1.5 秒后接近目标;目标回退时显示值不倒退。
TEST(InstallerTimeline, ApproachNeverOvershootsOrRegresses) {
    double shown = 0.0;
    double last = 0.0;
    for (int i = 0; i < 100; ++i) {
        shown = timeline::approach(shown, 0.8, 16.0);
        EXPECT_GE(shown, last);
        EXPECT_LE(shown, 0.8);
        EXPECT_LE(shown - last, 16.0 / timeline::kMinFillMs + 1e-12);
        last = shown;
    }
    EXPECT_GT(shown, 0.79);
    EXPECT_DOUBLE_EQ(timeline::approach(shown, 0.5, 16.0), shown);
}

// 场景:工作线程一下子报完成(目标 1),界面按 16ms 一帧推进。
// 期望:不到 kMinFillMs 不会到 1,保证安装中的水位上涨被看见。
TEST(InstallerTimeline, InstantInstallStillFillsGradually) {
    double shown = 0.0;
    int frames = 0;
    while (shown < 1.0 && frames < 1000) {
        shown = timeline::approach(shown, 1.0, 16.0);
        ++frames;
    }
    EXPECT_GE(frames * 16.0, timeline::kMinFillMs - 16.0);
}
