#pragma once


#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace acecode::agent::detail {

std::string human_bytes(std::size_t bytes);

std::string format_bytes_detail(std::size_t bytes);

} // namespace acecode::agent::detail
