#pragma once

#include <atomic>
#include <nlohmann/json.hpp>
#include <string>

namespace acecode::computer_use {

// One process-wide broker, one session lease; the helper also arbitrates across
// daemon processes on the same interactive desktop. Disabled until configured.
bool supported();
bool enabled();
void set_enabled(bool value);
// Appearance changes do not revoke observations or enable desktop control.
// Invalid styles/colors throw std::invalid_argument without changing state.
void set_pointer_appearance(const std::string& style, const std::string& color);
void release_session(const std::string& session_id);
void shutdown();
nlohmann::json execute(const std::string& session_id,
                       const nlohmann::json& request,
                       const std::atomic<bool>* abort_flag = nullptr);

} // namespace acecode::computer_use
