#pragma once

#include "backend.hpp"

#include <functional>

namespace acecode::web_search {

// Visit organic results in document order. Return false to stop at the limit.
void visit_so360_results(std::string_view html,
                         const std::function<bool(SearchHit)>& visit);

} // namespace acecode::web_search
