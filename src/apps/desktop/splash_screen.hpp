#pragma once

#include <cstdint>
#include <string>
#include <memory>

namespace acecode::desktop {

class SplashScreen {
public:
    SplashScreen();
    ~SplashScreen();

    SplashScreen(const SplashScreen&) = delete;
    SplashScreen& operator=(const SplashScreen&) = delete;

    void show();
    void set_status(const std::string& message, std::uint64_t elapsed_ms);
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acecode::desktop
