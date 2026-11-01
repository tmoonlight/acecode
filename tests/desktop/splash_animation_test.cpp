#include "desktop/splash_animation.hpp"
#include "desktop/splash_icon_painter_win.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#endif

using namespace acecode::desktop;
using namespace acecode::desktop::splash_anim;

namespace {

// 首屏在浮窗出现后 requested_at 毫秒就绪,此后又过了 elapsed 毫秒的那一帧。
Frame finishing(double requested_at, double elapsed) {
    return frame_at(requested_at + elapsed, elapsed);
}

}  // namespace

// 场景:Desktop 刚启动,浮窗出现。
// 期望:方砖从很小弹到原尺寸;随后波浪从底部升起、'A' 擦入、'>' 从右侧飞入并砸上,落地时有白热和辉光。
TEST(SplashAnimation, IntroGrowsTileThenAssemblesMark) {
    const Frame first = frame_at(0.0, std::nullopt);
    EXPECT_EQ(first.tile_opacity, 0.0);
    EXPECT_LT(first.tile_scale, 0.3);
    EXPECT_EQ(first.wave_rise, 0.0);
    EXPECT_EQ(first.a_reveal, 0.0);
    EXPECT_EQ(first.chevron_draw, 0.0);

    const Frame growing = frame_at(200.0, std::nullopt);
    EXPECT_GT(growing.tile_scale, 0.3);
    EXPECT_EQ(growing.wave_rise, 0.0);
    EXPECT_DOUBLE_EQ(frame_at(kTileGrowMs, std::nullopt).tile_scale, 1.0);

    // 波浪先于 'A' 出现。
    const Frame sliding = frame_at((kWaveRiseStartMs + kWaveRiseEndMs) / 2.0, std::nullopt);
    EXPECT_GT(sliding.wave_rise, 0.0);
    EXPECT_LT(sliding.wave_rise, 1.0);
    EXPECT_EQ(sliding.a_reveal, 0.0);

    const Frame letter = frame_at(kLetterEndMs, std::nullopt);
    EXPECT_DOUBLE_EQ(letter.wave_rise, 1.0);
    EXPECT_DOUBLE_EQ(letter.a_reveal, 1.0);
    EXPECT_EQ(letter.chevron_draw, 0.0);

    const Frame flying = frame_at((kChevronSlamMs + kChevronLandMs) / 2.0, std::nullopt);
    EXPECT_GT(flying.chevron_offset, 0.0);

    const Frame landed = frame_at(kChevronLandMs, std::nullopt);
    EXPECT_DOUBLE_EQ(landed.chevron_draw, 1.0);
    EXPECT_DOUBLE_EQ(landed.chevron_offset, 0.0);
    EXPECT_GT(landed.heat, 0.0);
    EXPECT_GT(landed.chevron_glow, 0.9);
}

// 场景:开场播完后,首屏还在读取(配置、工作区、后台服务……)。
// 期望:完整的 'A>' 静止显示,没有白热 / 辉光 / 震动残留;状态条与整窗不透明;不会提前要求显示主窗口。
TEST(SplashAnimation, LoadingAfterIntroShowsFullMarkWithRunningWave) {
    for (double t : {kIntroTotalMs + 200.0, 5000.0, 60000.0}) {
        const Frame frame = frame_at(t, std::nullopt);
        EXPECT_DOUBLE_EQ(frame.tile_scale, 1.0) << t;
        EXPECT_DOUBLE_EQ(frame.tile_opacity, 1.0) << t;
        EXPECT_DOUBLE_EQ(frame.wave_rise, 1.0) << t;
        EXPECT_DOUBLE_EQ(frame.a_reveal, 1.0) << t;
        EXPECT_DOUBLE_EQ(frame.chevron_draw, 1.0) << t;
        EXPECT_EQ(frame.chevron_offset, 0.0) << t;
        EXPECT_EQ(frame.heat, 0.0) << t;
        EXPECT_EQ(frame.chevron_glow, 0.0) << t;
        EXPECT_EQ(frame.shake_x, 0.0) << t;
        EXPECT_EQ(frame.wave_morph, 0.0) << t;
        EXPECT_EQ(frame.status_opacity, 1.0) << t;
        EXPECT_EQ(frame.window_opacity, 1.0) << t;
        EXPECT_FALSE(frame.reveal_main_window) << t;
        EXPECT_FALSE(frame.done) << t;
    }
}

// 场景:读取持续较久,波浪要「一直在跑」。
// 期望:同一个 x 处的波浪高度随时间变化;跑满一个周期后回到原样(循环无跳变)。
TEST(SplashAnimation, RunningWaveMovesAndLoopsSeamlessly) {
    const double x = 200.0;
    const auto y_at = [x](double t) {
        const Frame frame = frame_at(t, std::nullopt);
        return wave_y(x, frame.wave_phase, frame.wave_morph);
    };
    // 相隔半个周期,同一点的波浪高度关于中线对称翻转。
    EXPECT_GT(std::abs(y_at(2000.0) - y_at(2000.0 + kWavePeriodMs / 2.0)), 20.0);
    EXPECT_NEAR(y_at(2000.0), y_at(2000.0 + kWavePeriodMs), 1e-6);
    EXPECT_NEAR(y_at(2000.0), y_at(2000.0 + 37.0 * kWavePeriodMs), 1e-6);
}

// 场景:首屏就绪,浮窗收尾。
// 期望:波浪落回图标的静止形状(深色回到 12%),'A>' 保持完整;
// 收尾 kRevealMainWindowMs 才请求显示主窗口(此前主窗口保持隐藏),kFinishTotalMs 淡出完毕并结束。
TEST(SplashAnimation, FinishSettlesWaveThenHandsOff) {
    EXPECT_NEAR(resting_wave_y(20.0), 386.0, 1e-6);
    EXPECT_NEAR(resting_wave_y(310.0), 363.0, 1e-6);
    EXPECT_NEAR(resting_wave_y(492.0), 357.0, 1e-6);

    EXPECT_EQ(finishing(3000.0, 0.0).wave_morph, 0.0);
    for (double requested : {3000.0, 3333.0, 54321.0}) {
        const Frame settled = finishing(requested, kWaveSettleMs);
        EXPECT_DOUBLE_EQ(settled.wave_morph, 1.0);
        EXPECT_DOUBLE_EQ(settled.wave_opacity, 0.12);
        for (double x : {20.0, 150.0, 256.0, 400.0, 492.0}) {
            EXPECT_NEAR(wave_y(x, settled.wave_phase, settled.wave_morph), resting_wave_y(x), 1e-9);
        }
    }

    EXPECT_FALSE(finishing(3000.0, kRevealMainWindowMs - 1.0).reveal_main_window);
    EXPECT_DOUBLE_EQ(finishing(3000.0, kRevealMainWindowMs - 1.0).window_opacity, 1.0);
    EXPECT_TRUE(finishing(3000.0, kRevealMainWindowMs).reveal_main_window);
    const Frame done = finishing(3000.0, kFinishTotalMs);
    EXPECT_TRUE(done.done);
    EXPECT_DOUBLE_EQ(done.window_opacity, 0.0);
    EXPECT_DOUBLE_EQ(done.a_reveal, 1.0);
    EXPECT_DOUBLE_EQ(done.chevron_draw, 1.0);
}

// 场景:机器很快,首屏在开场动画还没播完时就绪(浮窗出现 600ms 后)。
// 期望:开场照常播完,收尾从 kIntroTotalMs 才开始,主窗口不会在 'A>' 组装完之前出现。
TEST(SplashAnimation, EarlyFinishWaitsForIntro) {
    const double requested = 600.0;
    const Frame mid_intro = finishing(requested, kChevronSlamMs - requested);
    EXPECT_FALSE(mid_intro.reveal_main_window);
    EXPECT_EQ(mid_intro.wave_morph, 0.0);
    EXPECT_EQ(mid_intro.status_opacity, 1.0);

    const double wait = kIntroTotalMs - requested;
    EXPECT_FALSE(finishing(requested, wait + kRevealMainWindowMs - 1.0).reveal_main_window);
    const Frame reveal = finishing(requested, wait + kRevealMainWindowMs);
    EXPECT_TRUE(reveal.reveal_main_window);
    EXPECT_DOUBLE_EQ(reveal.a_reveal, 1.0);
    EXPECT_DOUBLE_EQ(reveal.chevron_draw, 1.0);
    EXPECT_TRUE(finishing(requested, wait + kFinishTotalMs).done);
}

// 场景:'>' 砸上前的两次预告(对应宣传片 k79.5 / k79.75 的拾音)。
// 期望:预告只在原位描出一截(第二次比第一次长),不发生位移;预告之间 '>' 消失。
TEST(SplashAnimation, ChevronTeasesInPlaceBeforeSlam) {
    const Frame first = frame_at(kChevronTease1Ms, std::nullopt);
    const Frame second = frame_at(kChevronTease2Ms, std::nullopt);
    const Frame between = frame_at((kChevronTease1Ms + kChevronTease2Ms) / 2.0, std::nullopt);
    EXPECT_NEAR(first.chevron_draw, 0.3, 1e-9);
    EXPECT_NEAR(second.chevron_draw, 0.5, 1e-9);
    EXPECT_EQ(first.chevron_offset, 0.0);
    EXPECT_EQ(second.chevron_offset, 0.0);
    EXPECT_EQ(between.chevron_draw, 0.0);
}

// 场景:落地震动按帧取伪随机数。
// 期望:同一时刻两次计算结果相同(重绘不会抖出不同画面);落地前和落地 190ms 后都不震。
TEST(SplashAnimation, ShakeIsDeterministicAndShortLived) {
    const Frame a = frame_at(kChevronLandMs + 20.0, std::nullopt);
    const Frame b = frame_at(kChevronLandMs + 20.0, std::nullopt);
    EXPECT_EQ(a.shake_x, b.shake_x);
    EXPECT_EQ(a.shake_y, b.shake_y);
    EXPECT_GT(std::abs(a.shake_x) + std::abs(a.shake_y), 0.0);
    const Frame before = frame_at(kChevronLandMs - 1.0, std::nullopt);
    EXPECT_EQ(before.shake_x, 0.0);
    const Frame after = frame_at(kChevronLandMs + 190.0, std::nullopt);
    EXPECT_EQ(after.shake_x, 0.0);
    EXPECT_EQ(after.shake_y, 0.0);
}

// 场景:'>' 的描边进度要沿 SVG 折线 M348 203 L431 256 L348 309 走。
// 期望:0 在起点、0.5 在尖角、1 在终点。
TEST(SplashAnimation, ChevronPathFollowsIconPolyline) {
    EXPECT_EQ(chevron_point_at(0.0), (std::array<double, 2>{348.0, 203.0}));
    EXPECT_EQ(chevron_point_at(0.5), (std::array<double, 2>{431.0, 256.0}));
    EXPECT_EQ(chevron_point_at(1.0), (std::array<double, 2>{348.0, 309.0}));
}

#ifdef _WIN32
namespace {

constexpr int kIcon = 256;

struct Pixel {
    int r, g, b, a;
};

// 在不透明深灰底上画一帧,方便取像素和人工查看。
std::vector<std::uint32_t> render_frame(const Frame& frame) {
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(kIcon) * kIcon, 0xFF262626u);
    EXPECT_TRUE(paint_splash_icon(pixels.data(), kIcon, kIcon, 0.0, 0.0, kIcon, frame));
    return pixels;
}

// 取视口坐标 (vx, vy) 处的像素。
Pixel pixel_at(const std::vector<std::uint32_t>& pixels, double vx, double vy) {
    const int x = static_cast<int>(vx * kIcon / 512.0);
    const int y = static_cast<int>(vy * kIcon / 512.0);
    const std::uint32_t p = pixels[static_cast<std::size_t>(y) * kIcon + x];
    return {static_cast<int>((p >> 16) & 0xFF), static_cast<int>((p >> 8) & 0xFF),
            static_cast<int>(p & 0xFF), static_cast<int>(p >> 24)};
}

bool png_encoder(CLSID* clsid) {
    UINT count = 0, bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0) return false;
    std::vector<unsigned char> buffer(bytes);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < count; ++i) {
        if (std::wstring(codecs[i].MimeType) == L"image/png") {
            *clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

}  // namespace

// 场景:开场三个时刻与读取中各画一帧。
// 期望:刚出现时方砖还小,左侧仍是底色;方砖长成、'A' 未出现时,'A' 左腿与 '>' 的位置是方砖蓝;
// 读取中 'A' 左腿接近白色、'>' 是浅青色,'A' 的三角镂空露出方砖蓝,圆角外保持底色。
// 回归:GDI+ 渐变 / 裁切 / evenodd / 缩放任一处写错,都会让组装出的 'A>' 与应用图标对不上。
TEST(SplashIconPainter, DrawsIntroStagesAndFullMark) {
    SplashGraphicsRuntime runtime;
    ASSERT_TRUE(runtime.ok());

    const auto tiny = render_frame(frame_at(60.0, std::nullopt));
    EXPECT_EQ(pixel_at(tiny, 60.0, 256.0).r, 0x26);

    const auto grown = render_frame(frame_at(kTileGrowMs + 40.0, std::nullopt));
    const Pixel leg_grown = pixel_at(grown, 115.0, 350.0);
    const Pixel chevron_grown = pixel_at(grown, 410.0, 243.0);
    EXPECT_LT(leg_grown.r, 120);
    EXPECT_GT(leg_grown.b, 150);
    EXPECT_LT(chevron_grown.g, 150);

    const auto loading = render_frame(frame_at(2000.0, std::nullopt));
    const Pixel leg = pixel_at(loading, 115.0, 350.0);
    const Pixel chevron = pixel_at(loading, 410.0, 243.0);
    EXPECT_GT(leg.r, 220);
    EXPECT_GT(leg.g, 230);
    EXPECT_GT(chevron.g, 200);
    EXPECT_GT(chevron.b, 220);
    EXPECT_LT(pixel_at(loading, 200.0, 250.0).r, 120);

    const Pixel corner = pixel_at(loading, 24.0, 24.0);
    EXPECT_EQ(corner.r, 0x26);
    EXPECT_EQ(corner.a, 0xFF);
}

// 人工查看用:设置 ACECODE_SPLASH_DUMP_DIR 时,把开场、读取与收尾的逐帧画面拼成一张 PNG 写进该目录。
TEST(SplashIconPainter, DumpsContactSheetWhenRequested) {
    const char* dir = std::getenv("ACECODE_SPLASH_DUMP_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "ACECODE_SPLASH_DUMP_DIR not set";
    SplashGraphicsRuntime runtime;
    ASSERT_TRUE(runtime.ok());

    std::vector<Frame> frames;
    for (double t = 0.0; t <= kIntroTotalMs; t += 65.0) frames.push_back(frame_at(t, std::nullopt));
    for (double t : {1800.0, 2200.0}) frames.push_back(frame_at(t, std::nullopt));
    for (double f = 0.0; f <= kFinishTotalMs; f += 60.0) frames.push_back(finishing(3000.0, f));
    constexpr int kColumns = 6;
    const int rows = static_cast<int>((frames.size() + kColumns - 1) / kColumns);
    const int width = kColumns * kIcon;
    const int height = rows * kIcon;
    std::vector<std::uint32_t> sheet(static_cast<std::size_t>(width) * height, 0xFF262626u);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const int ox = static_cast<int>(i % kColumns) * kIcon;
        const int oy = static_cast<int>(i / kColumns) * kIcon;
        const auto cell = render_frame(frames[i]);
        for (int y = 0; y < kIcon; ++y) {
            for (int x = 0; x < kIcon; ++x) {
                sheet[static_cast<std::size_t>(oy + y) * width + ox + x] =
                    cell[static_cast<std::size_t>(y) * kIcon + x];
            }
        }
    }
    Gdiplus::Bitmap bitmap(width, height, width * 4, PixelFormat32bppPARGB,
                           reinterpret_cast<BYTE*>(sheet.data()));
    CLSID png{};
    ASSERT_TRUE(png_encoder(&png));
    const std::wstring path = std::wstring(dir, dir + std::strlen(dir)) + L"\\splash_frames.png";
    EXPECT_EQ(bitmap.Save(path.c_str(), &png, nullptr), Gdiplus::Ok);
}
#endif
