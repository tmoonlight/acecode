#pragma once

#include "splash_animation.hpp"

#include <cstdint>

#ifdef _WIN32

namespace acecode::desktop {

// GDI+ 运行期。画图标前,同一线程上持有一个即可。
class SplashGraphicsRuntime {
public:
    SplashGraphicsRuntime();
    ~SplashGraphicsRuntime();
    SplashGraphicsRuntime(const SplashGraphicsRuntime&) = delete;
    SplashGraphicsRuntime& operator=(const SplashGraphicsRuntime&) = delete;
    bool ok() const { return token_ != 0; }

private:
    std::uintptr_t token_ = 0;
};

// 把一帧启动图标画进 32 位预乘 BGRA 缓冲(行距 width*4,自上而下),叠加在已有内容之上。
// origin_x / origin_y / size 是 512 视口在缓冲里的位置与边长(像素)。
bool paint_splash_icon(std::uint32_t* pixels,
                       int width,
                       int height,
                       double origin_x,
                       double origin_y,
                       double size,
                       const splash_anim::Frame& frame);

}  // namespace acecode::desktop

#endif
