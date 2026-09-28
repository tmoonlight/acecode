#pragma once

#include <cstdint>
#include <string>

namespace acecode::desktop {

class SplashScreen {
public:
    SplashScreen() = default;
    ~SplashScreen();

    SplashScreen(const SplashScreen&) = delete;
    SplashScreen& operator=(const SplashScreen&) = delete;

    void show();
    void set_status(const std::string& message, std::uint64_t elapsed_ms);
    void close();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace acecode::desktop
