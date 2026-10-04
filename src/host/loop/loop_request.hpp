#pragma once

#include "loop_types.hpp"

#include <functional>
#include <optional>

namespace acecode::loop {

// The resolver is borrowed synchronously; this function never retains it.
bool parse_loop_request(const nlohmann::json& body,
                        std::int64_t now_ms,
                        const std::vector<std::string>& model_names,
                        const std::function<std::optional<std::string>(const std::string&)>& workspace_cwd,
                        LoopDefinition& out,
                        ValidationError& error);

} // namespace acecode::loop
