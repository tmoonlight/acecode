#include "installer_timeline.hpp"

#include <algorithm>
#include <cmath>

namespace acecode::installer::timeline {
namespace {

constexpr double kRunningWaveOpacity = 0.2;
constexpr double kIconWaveOpacity = 0.12;
constexpr double kChevronExitDistance = 300.0 * 512.0 / 360.0;
constexpr double kShakeAmplitude = 6.0 * 512.0 / 360.0;
constexpr double kFillWaveOpacity = 0.34;

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
double progress(double t, double from, double to) { return clamp01((t - from) / (to - from)); }
double lerp(double a, double b, double t) { return a + (b - a) * t; }
double ease_out(double p) { return 1.0 - std::pow(1.0 - p, 3.0); }
double ease_in(double p) { return p * p; }
double ease_in_out(double p) {
    return p < 0.5 ? 4.0 * p * p * p : 1.0 - std::pow(-2.0 * p + 2.0, 3.0) / 2.0;
}
double hit(double t, double at, double duration) {
    if (t < at) return 0.0;
    const double rest = 1.0 - clamp01((t - at) / duration);
    return rest * rest;
}
double frame_noise(int n) {
    const double s = std::sin(static_cast<double>(n) * 12.9898) * 43758.5453;
    return s - std::floor(s);
}

void shake(splash_anim::Frame& frame, double t, double amplitude) {
    if (amplitude <= 0.0) return;
    const int n = static_cast<int>(t / (1000.0 / 60.0));
    frame.shake_x = (frame_noise(2 * n + 1) - 0.5) * 2.0 * amplitude;
    frame.shake_y = (frame_noise(2 * n + 2) - 0.5) * 2.0 * amplitude;
}

// 开场早已播完时的「整图标 + 跑动波浪」。clock_ms 很小时也直接给完整图标,
// 防止用户在开场没播完时就切进下一个场景。
splash_anim::Frame steady(double clock_ms) {
    auto frame = splash_anim::frame_at(std::max(clock_ms, splash_anim::kIntroTotalMs),
                                       std::nullopt);
    frame.wave_phase = std::fmod(2.0 * 3.14159265358979323846 * std::max(0.0, clock_ms) /
                                     splash_anim::kWavePeriodMs,
                                 2.0 * 3.14159265358979323846);
    frame.heat = 0.0;
    frame.chevron_glow = 0.0;
    frame.shake_x = 0.0;
    frame.shake_y = 0.0;
    return frame;
}

double rise_for_level(double level) { return 1.0 + kFillTravel * clamp01(level); }

}  // namespace

splash_anim::Frame welcome(double clock_ms) {
    return splash_anim::frame_at(std::max(0.0, clock_ms), std::nullopt);
}

double content_reveal(double clock_ms) {
    return ease_out(progress(clock_ms, kContentRevealStartMs, kContentRevealEndMs));
}

double page_reveal(double scene_ms) { return ease_out(progress(scene_ms, 0.0, 260.0)); }

splash_anim::Frame installing(double clock_ms, double level) {
    auto frame = steady(clock_ms);
    frame.wave_rise = rise_for_level(level);
    frame.wave_opacity = lerp(kRunningWaveOpacity, kFillWaveOpacity, clamp01(level));
    return frame;
}

splash_anim::Frame finished(double scene_ms, double clock_ms, double from_level) {
    const double t = std::max(0.0, scene_ms);
    auto frame = steady(clock_ms);
    const double drain = ease_in_out(progress(t, 0.0, 700.0));
    const double level = clamp01(from_level) * (1.0 - drain);
    frame.wave_rise = rise_for_level(level);
    frame.wave_morph = ease_out(progress(t, 480.0, kFinishCelebrateMs));
    const double settled_opacity = lerp(kRunningWaveOpacity, kIconWaveOpacity, frame.wave_morph);
    frame.wave_opacity = lerp(settled_opacity,
                              lerp(kRunningWaveOpacity, kFillWaveOpacity, clamp01(from_level)),
                              1.0 - drain);
    frame.heat = 0.45 * hit(t, kFinishCelebrateMs, 110.0);
    frame.chevron_glow = hit(t, kFinishCelebrateMs, 360.0);
    shake(frame, t, 0.6 * kShakeAmplitude * hit(t, kFinishCelebrateMs, 190.0));
    return frame;
}

splash_anim::Frame failed(double scene_ms, double clock_ms, double from_level) {
    const double t = std::max(0.0, scene_ms);
    auto frame = steady(clock_ms);
    const double drain = ease_in_out(progress(t, 0.0, 500.0));
    frame.wave_rise = rise_for_level(clamp01(from_level) * (1.0 - drain));
    frame.wave_morph = ease_out(progress(t, 260.0, 700.0));
    frame.wave_opacity = lerp(kRunningWaveOpacity, kIconWaveOpacity, frame.wave_morph);
    shake(frame, t, 0.5 * kShakeAmplitude * hit(t, 0.0, 220.0));
    return frame;
}

splash_anim::Frame uninstalling(double clock_ms, double progress_value) {
    auto frame = steady(clock_ms);
    frame.wave_rise = 1.0 - clamp01(progress_value);
    return frame;
}

splash_anim::Frame removed(double scene_ms) {
    const double t = std::max(0.0, scene_ms);
    auto frame = steady(0.0);
    frame.wave_rise = 0.0;
    const double fly = ease_in(progress(t, 0.0, 260.0));
    frame.chevron_offset = kChevronExitDistance * fly;
    frame.chevron_draw = 1.0 - progress(t, 140.0, 260.0);
    frame.a_reveal = 1.0 - ease_in_out(progress(t, 200.0, 380.0));
    const double fade = ease_in_out(progress(t, 320.0, kRemovedTotalMs));
    frame.tile_scale = 1.0 - 0.32 * fade;
    frame.tile_opacity = 1.0 - 0.65 * fade;
    return frame;
}

double approach(double displayed, double target, double dt_ms) {
    if (target <= displayed) return displayed;
    const double dt = std::max(0.0, dt_ms);
    const double k = 1.0 - std::exp(-dt / 120.0);
    const double next = displayed + (target - displayed) * k + 0.0005;
    return std::min({target, next, displayed + dt / kMinFillMs});
}

}  // namespace acecode::installer::timeline
