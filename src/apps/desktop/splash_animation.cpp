#include "splash_animation.hpp"

#include <algorithm>
#include <cmath>

namespace acecode::desktop::splash_anim {

namespace {

constexpr double kPi = 3.14159265358979323846;

// 静止波浪 = 两段三次贝塞尔(取自 acecode-icon.svg 的波浪路径),x 在每段内单调。
struct Cubic {
    double x0, y0, x1, y1, x2, y2, x3, y3;
};
constexpr Cubic kRestingWave[] = {
    {20, 386, 126, 326, 223, 326, 310, 363},
    {310, 363, 378, 392, 438, 391, 492, 357},
};

// 奔跑波浪:一个波长横跨方砖,中线与振幅贴近静止波浪,收尾插值时不会突然跳动。
constexpr double kRunningMidline = 364.0;
constexpr double kRunningAmplitude = 22.0;
constexpr double kRunningWaveOpacity = 0.2;
constexpr double kIconWaveOpacity = 0.12;

// '>' 从右侧飞入的距离:宣传片里图标 360px 时飞 300px,换算成视口单位。
constexpr double kChevronFlyDistance = 300.0 * 512.0 / 360.0;
// 落地震动:宣传片 6px / 360px 图标。
constexpr double kShakeAmplitude = 6.0 * 512.0 / 360.0;

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
double lerp(double a, double b, double t) { return a + (b - a) * t; }
double progress(double t, double from, double to) { return clamp01((t - from) / (to - from)); }

double ease_out(double p) { return 1.0 - std::pow(1.0 - p, 3.0); }
double ease_in(double p) { return p * p; }
double ease_in_out(double p) {
    return p < 0.5 ? 4.0 * p * p * p : 1.0 - std::pow(-2.0 * p + 2.0, 3.0) / 2.0;
}
// 轻微过冲,方砖出现时用。
double ease_out_back(double p) {
    constexpr double c1 = 1.2;
    constexpr double c3 = c1 + 1.0;
    const double q = p - 1.0;
    return 1.0 + c3 * q * q * q + c1 * q * q;
}

// 以 at 为峰的三角脉冲,rise / fall 为上升 / 下降时长。
double pulse(double t, double at, double rise, double fall) {
    const double d = t - at;
    if (d < -rise || d > fall) return 0.0;
    return d < 0.0 ? 1.0 + d / rise : 1.0 - d / fall;
}

// at 时刻打满,随后在 duration 内平方衰减到 0。
double hit(double t, double at, double duration) {
    if (t < at) return 0.0;
    const double rest = 1.0 - clamp01((t - at) / duration);
    return rest * rest;
}

// 逐帧确定的伪随机数 [0, 1),震动用,同一帧总是同一个值。
double frame_noise(int n) {
    const double s = std::sin(static_cast<double>(n) * 12.9898) * 43758.5453;
    return s - std::floor(s);
}

double cubic_at(double a, double b, double c, double d, double t) {
    const double u = 1.0 - t;
    return u * u * u * a + 3.0 * u * u * t * b + 3.0 * u * t * t * c + t * t * t * d;
}

}  // namespace

double resting_wave_y(double x) {
    const Cubic& seg = x <= kRestingWave[0].x3 ? kRestingWave[0] : kRestingWave[1];
    const double target = std::clamp(x, seg.x0, seg.x3);
    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < 40; ++i) {
        const double mid = (lo + hi) * 0.5;
        if (cubic_at(seg.x0, seg.x1, seg.x2, seg.x3, mid) < target) lo = mid;
        else hi = mid;
    }
    return cubic_at(seg.y0, seg.y1, seg.y2, seg.y3, (lo + hi) * 0.5);
}

double running_wave_y(double x, double phase) {
    return kRunningMidline +
           kRunningAmplitude * std::sin(2.0 * kPi * (x - kTileX) / kTileSize - phase);
}

double wave_y(double x, double phase, double morph) {
    const double m = clamp01(morph);
    if (m >= 1.0) return resting_wave_y(x);
    if (m <= 0.0) return running_wave_y(x, phase);
    return lerp(running_wave_y(x, phase), resting_wave_y(x), m);
}

double a_clip_top(double reveal) { return lerp(398.0, 104.0, clamp01(reveal)); }

std::array<double, 2> chevron_point_at(double progress_value) {
    const double q = clamp01(progress_value);
    if (q <= 0.5) {
        const double f = q * 2.0;
        return {348.0 + 83.0 * f, 203.0 + 53.0 * f};
    }
    const double f = (q - 0.5) * 2.0;
    return {431.0 - 83.0 * f, 256.0 + 53.0 * f};
}

Frame frame_at(double loading_ms, std::optional<double> finish_ms) {
    Frame frame;
    const double t = std::max(0.0, loading_ms);

    // 方砖从小弹到原尺寸(带一点过冲),同时淡入。
    frame.tile_opacity = progress(t, 0.0, 120.0);
    frame.tile_scale = 0.15 + 0.85 * ease_out_back(progress(t, 0.0, kTileGrowMs));
    // 波浪从方砖底部升起,之后一直向右跑。
    frame.wave_rise = ease_out(progress(t, kWaveRiseStartMs, kWaveRiseEndMs));
    frame.wave_phase = std::fmod(2.0 * kPi * t / kWavePeriodMs, 2.0 * kPi);
    frame.a_reveal = ease_in_out(progress(t, kLetterStartMs, kLetterEndMs));

    // '>':两次小幅预告(只描出一截,不移动),然后从右侧飞入并一次描完。
    const double tease = std::max(0.3 * pulse(t, kChevronTease1Ms, 25.0, 25.0),
                                  0.5 * pulse(t, kChevronTease2Ms, 25.0, 25.0));
    const double slam = ease_out(progress(t, kChevronSlamMs, kChevronLandMs));
    frame.chevron_draw = std::max(tease, slam);
    frame.chevron_offset = slam > tease ? kChevronFlyDistance * (1.0 - slam) : 0.0;

    // 落地:白热一闪、辉光衰减、轻微震动。
    frame.heat = 0.45 * hit(t, kChevronLandMs, 110.0);
    frame.chevron_glow = hit(t, kChevronLandMs, 320.0);
    const double shake = kShakeAmplitude * hit(t, kChevronLandMs, 190.0);
    if (shake > 0.0) {
        const int n = static_cast<int>(t / (1000.0 / 60.0));
        frame.shake_x = (frame_noise(2 * n + 1) - 0.5) * 2.0 * shake;
        frame.shake_y = (frame_noise(2 * n + 2) - 0.5) * 2.0 * shake;
    }
    if (!finish_ms.has_value()) return frame;

    // 收尾:开场播完之后才开始,波浪落回图标的静止形状,然后交给主窗口并淡出。
    const double requested_at = t - std::max(0.0, *finish_ms);
    const double f = t - std::max(requested_at, kIntroTotalMs);
    if (f < 0.0) return frame;
    frame.wave_morph = ease_out(progress(f, 0.0, kWaveSettleMs));
    frame.wave_opacity = lerp(kRunningWaveOpacity, kIconWaveOpacity, frame.wave_morph);
    frame.status_opacity = 1.0 - progress(f, 0.0, 200.0);
    frame.reveal_main_window = f >= kRevealMainWindowMs;
    frame.window_opacity = 1.0 - ease_in(progress(f, kRevealMainWindowMs, kFinishTotalMs));
    frame.done = f >= kFinishTotalMs;
    return frame;
}

}  // namespace acecode::desktop::splash_anim
