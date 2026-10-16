// The helper's framed stdout writer (helper process only; the Desktop shell
// links the shared simulator code without it).
#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace acecode::ios_simulator::helper {

// Serialized and safe from any queue. Frames with drop_if_busy are skipped
// while more than a couple of droppable frames are pending.
void emit(const nlohmann::json& header, const std::string& binary = {}, bool drop_if_busy = false);
void emit_log(const std::string& level, const std::string& message);

} // namespace acecode::ios_simulator::helper
