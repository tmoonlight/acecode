#pragma once

#include "llm/llm_provider.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace acecode::agent::detail {

std::string provider_error_kind_to_json_string(ProviderErrorKind kind);

nlohmann::json provider_error_to_json(const ProviderErrorInfo& info);

std::string provider_error_summary_for_log(const ProviderErrorInfo& info);

} // namespace acecode::agent::detail
