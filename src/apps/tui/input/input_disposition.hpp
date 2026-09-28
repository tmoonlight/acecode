#pragma once
#include <optional>

namespace acecode::tui {
enum class InputDisposition { Continue, Consumed, Declined };
inline std::optional<bool> input_result(InputDisposition disposition) {
    if (disposition == InputDisposition::Continue) return std::nullopt;
    return disposition == InputDisposition::Consumed;
}
inline InputDisposition input_handled(bool handled) {
    return handled ? InputDisposition::Consumed : InputDisposition::Continue;
}
inline InputDisposition input_stopped(bool consumed) {
    return consumed ? InputDisposition::Consumed : InputDisposition::Declined;
}
}
