#pragma once

// 安装界面的图标动画,在启动浮窗时间轴(desktop/splash_animation)上延伸:
//   欢迎页   = 启动浮窗开场(方砖弹出、波浪升起、'A' 擦入、'>' 砸上),之后波浪一直跑;
//   安装中   = 方砖里的水位随进度上涨;
//   完成     = 水退回图标里的静止波浪,'>' 再亮一次;
//   失败     = 水退回、轻震一下后静止;
//   卸载中   = 水位随进度退到方砖以下;
//   已卸载   = '>' 向右飞出并收回,'A' 自上而下擦掉,方砖缩小变淡。
// 纯计算,没有平台依赖。scene_ms 从该场景开始算,clock_ms 从窗口出现算(波浪相位用它,
// 场景切换时波浪不跳)。

#include "desktop/splash_animation.hpp"

namespace acecode::installer::timeline {

namespace splash_anim = ::acecode::desktop::splash_anim;

// 安装进度 1 时水面中线从图标原位(y≈364)涨到约 y≈150。
inline constexpr double kFillTravel = (364.0 - 150.0) / (512.0 - 330.0);
// 开场播完、欢迎页内容开始淡入的时刻。
inline constexpr double kContentRevealStartMs = 520.0;
inline constexpr double kContentRevealEndMs = 900.0;
inline constexpr double kFinishCelebrateMs = 900.0;
inline constexpr double kRemovedTotalMs = 720.0;
// 显示进度从 0 到 1 最快用时:本机装完 45 MB 只要半秒,不设下限水位刚涨就切到完成页。
inline constexpr double kMinFillMs = 1500.0;

splash_anim::Frame welcome(double clock_ms);
// 欢迎页文字、按钮的淡入 0→1。
double content_reveal(double clock_ms);
// 场景切换后新页面内容的淡入 0→1。
double page_reveal(double scene_ms);

splash_anim::Frame installing(double clock_ms, double level);
splash_anim::Frame finished(double scene_ms, double clock_ms, double from_level);
splash_anim::Frame failed(double scene_ms, double clock_ms, double from_level);
splash_anim::Frame uninstalling(double clock_ms, double progress);
splash_anim::Frame removed(double scene_ms);

// 显示进度向目标进度靠拢(约 120ms 时间常数,速度不超过 1/kMinFillMs),不超过目标、不倒退。
double approach(double displayed, double target, double dt_ms);

}  // namespace acecode::installer::timeline
