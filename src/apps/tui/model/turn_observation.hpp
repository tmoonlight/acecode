#pragma once
#include <string>
namespace acecode::tui {
// Written/read under TuiState::mu; deltas do not count as final assistant text.
struct TurnObservation {
    std::string assistant_text;
    std::string outcome;
};
inline constexpr char kTuiMainPowerSessionId[] = "tui-main";
}
