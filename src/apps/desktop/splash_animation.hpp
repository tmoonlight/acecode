#pragma once

// 启动浮窗的图标动画时间轴(取自宣传片收尾镜头 s16 的图标组装,时长压缩到启动场景)。
// 开场:方砖从小弹到原尺寸 → 波浪从底部升起 → 'A' 自下而上擦入 → '>' 两次预告后从右侧砸上
// (白热、辉光、轻微震动)。之后整个读取期间显示完整的 'A>',只有波浪一直向右跑。
// 首屏就绪(且开场已播完)后:波浪落回图标里的静止形状 → 交给主窗口并淡出。
// 纯计算,没有平台依赖;坐标都是图标 512 视口单位,与 assets/branding/acecode-icon.svg 一致。

#include <array>
#include <optional>

namespace acecode::desktop::splash_anim {

inline constexpr double kTileX = 20.0;
inline constexpr double kTileY = 20.0;
inline constexpr double kTileSize = 472.0;
inline constexpr double kTileRadius = 112.0;
inline constexpr double kTileCenter = 256.0;

// 读取期间波浪向右跑一个波长所需的时间。
inline constexpr double kWavePeriodMs = 1600.0;

// 开场节点,毫秒,从浮窗出现算起。
inline constexpr double kTileGrowMs = 420.0;
inline constexpr double kWaveRiseStartMs = 320.0;
inline constexpr double kWaveRiseEndMs = 620.0;
// 'A' 擦入只给 85ms:机器卡的时候帧很稀,擦入拖长了会停在只露出两条腿的中间状态。
inline constexpr double kLetterStartMs = 560.0;
inline constexpr double kLetterEndMs = 645.0;
// '>' 紧跟 'A' 之后(间隔与擦入改快前相同),中间不留停顿。
inline constexpr double kChevronTease1Ms = 685.0;
inline constexpr double kChevronTease2Ms = 745.0;
inline constexpr double kChevronSlamMs = 795.0;
inline constexpr double kChevronLandMs = 880.0;
// 开场播完的时刻(落地辉光基本散去);首屏就算更早就绪,收尾也从这里才开始。
inline constexpr double kIntroTotalMs = 1045.0;

// 收尾节点,毫秒,从收尾开始算起。
inline constexpr double kWaveSettleMs = 260.0;
inline constexpr double kRevealMainWindowMs = 280.0;
inline constexpr double kFinishTotalMs = 430.0;

struct Frame {
    double tile_scale = 1.0;       // 方砖以砖心缩放
    double tile_opacity = 1.0;
    double wave_phase = 0.0;       // 奔跑波浪的相位(弧度)
    double wave_rise = 0.0;        // 波浪从方砖底部升起 0→1(0 不画)
    double wave_morph = 0.0;       // 0 = 奔跑的正弦波,1 = 图标里的静止波浪
    double wave_opacity = 0.2;     // 波浪深色的不透明度:读取时加深便于看出在跑,收尾落回图标的 12%
    double a_reveal = 0.0;         // 'A' 自下而上擦入 0→1
    double chevron_draw = 0.0;     // '>' 描边进度 0→1
    double chevron_offset = 0.0;   // '>' 向右的偏移(视口单位)
    double chevron_glow = 0.0;     // '>' 辉光 0→1
    double heat = 0.0;             // 方砖白热 0→1
    double shake_x = 0.0;          // 整个图标的震动偏移(视口单位)
    double shake_y = 0.0;
    double status_opacity = 1.0;   // 下方状态条
    double window_opacity = 1.0;   // 整个浮窗(交给主窗口后淡出)
    bool reveal_main_window = false;
    bool done = false;
};

// loading_ms:浮窗出现至今;finish_ms:首屏就绪至今,未就绪时为空。
// 首屏在开场播完前就绪时,收尾推迟到 kIntroTotalMs 才开始。
Frame frame_at(double loading_ms, std::optional<double> finish_ms);

// 图标里静止波浪的上沿(x ∈ [20, 492])。
double resting_wave_y(double x);
// 读取期间奔跑波浪的上沿。
double running_wave_y(double x, double phase);
// 两者按 morph 插值。
double wave_y(double x, double phase, double morph);

// 'A' 擦入时裁切矩形的上沿:reveal 0 时在字形下方,1 时在字形上方。
double a_clip_top(double reveal);

// '>' 两段折线的笔头位置,progress 为描边进度 0→1。
std::array<double, 2> chevron_point_at(double progress);

}  // namespace acecode::desktop::splash_anim
