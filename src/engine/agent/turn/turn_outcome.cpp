#include "turn_outcome.hpp"

#include <utility>

namespace acecode::agent {

void TurnOutcomeRecord::begin() {
    outcome_.store(Outcome::None, std::memory_order_release);
    set_error({});
}

void TurnOutcomeRecord::record(const std::string& status) {
    auto outcome = Outcome::Completed;
    if (status == "error") outcome = Outcome::Error;
    else if (status == "aborted") outcome = Outcome::Aborted;
    outcome_.store(outcome, std::memory_order_release);
}

void TurnOutcomeRecord::set_error(std::string error) {
    std::lock_guard<std::mutex> lock(error_mu_);
    error_ = std::move(error);
}

std::string TurnOutcomeRecord::error() const {
    std::lock_guard<std::mutex> lock(error_mu_);
    return error_;
}

} // namespace acecode::agent
