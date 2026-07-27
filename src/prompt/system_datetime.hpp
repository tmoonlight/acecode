#pragma once

#include <ctime>
#include <string>

namespace acecode {

std::string format_utc_offset(int offset_minutes);
std::string format_prompt_datetime(const std::tm& local_time,
                                   int utc_offset_minutes);
std::string current_prompt_datetime();

// Date-only variant used by the static system prompt. Deliberately omits the
// time of day: a coding agent needs the calendar date to reason correctly
// (library recency, changelog years, relative deadlines) but never needs
// second precision, and a per-request timestamp would change the cacheable
// prompt prefix on every single provider call.
std::string format_prompt_date(const std::tm& local_time,
                               int utc_offset_minutes);
std::string current_prompt_date();

} // namespace acecode
