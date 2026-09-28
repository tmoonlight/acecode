#pragma once

#include <cstdint>
#include <string>

namespace acecode { namespace tui {

inline std::string model_retry_wait_phrase(bool chinese,
                                           std::int64_t retry_delay_ms) {
    const std::int64_t clamped_ms =
        retry_delay_ms > 0 ? retry_delay_ms : 0;
    const std::int64_t seconds = (clamped_ms + 999) / 1000;

    std::string delay;
    if (seconds >= 60) {
        delay = std::to_string((seconds + 59) / 60) +
            (chinese ? " 分钟" : " min");
    } else {
        delay = std::to_string(seconds) +
            (chinese ? " 秒" : " sec");
    }

    return chinese
        ? "网络暂时不可用，" + delay + "后重试"
        : "Network unavailable, retrying in " + delay;
}

inline std::string model_retry_resume_phrase(bool chinese) {
    return chinese ? "正在重新连接" : "Reconnecting";
}

}} // namespace acecode::tui
