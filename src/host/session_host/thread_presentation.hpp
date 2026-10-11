#pragma once

#include <cstddef>
#include <string>
#include <nlohmann/json_fwd.hpp>

namespace acecode {
struct SessionMeta;
struct SessionInfo;
struct SessionEvent;
}

namespace acecode::thread_presentation {

std::string trim_and_limit(std::string value, std::size_t max_chars,
                           bool* truncated = nullptr);
nlohmann::json thread_summary(const SessionMeta& meta, const SessionInfo* active,
                              bool pinned, int pinned_index = 0);
nlohmann::json compact_wait_event(const SessionEvent& event);

} // namespace acecode::thread_presentation
