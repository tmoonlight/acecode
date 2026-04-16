#pragma once

#include <string>
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace acecode {

// Generate a unique identifier string (UUID v4-like format)
inline std::string generate_uuid() {
    static thread_local std::mt19937 rng(
        static_cast<unsigned int>(
            std::chrono::steady_clock::now().time_since_epoch().count()));

    std::uniform_int_distribution<int> dist(0, 15);
    std::uniform_int_distribution<int> dist2(8, 11); // variant bits

    std::ostringstream ss;
    ss << std::hex;
    for (int i = 0; i < 8; ++i) ss << dist(rng);
    ss << '-';
    for (int i = 0; i < 4; ++i) ss << dist(rng);
    ss << "-4"; // version 4
    for (int i = 0; i < 3; ++i) ss << dist(rng);
    ss << '-';
    ss << dist2(rng); // variant
    for (int i = 0; i < 3; ++i) ss << dist(rng);
    ss << '-';
    for (int i = 0; i < 12; ++i) ss << dist(rng);

    return ss.str();
}

// Get current ISO 8601 timestamp
inline std::string iso_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm buf;
#ifdef _WIN32
    gmtime_s(&buf, &t);
#else
    gmtime_r(&t, &buf);
#endif
    std::ostringstream ss;
    ss << std::put_time(&buf, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

} // namespace acecode
