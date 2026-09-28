#pragma once

#include <chrono>
#include <cstdint>

namespace acecode::utils {

inline std::int64_t now_epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace acecode::utils
