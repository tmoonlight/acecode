#pragma once

#include "config/config.hpp"

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

// Opaque connection identity: never persist credentials or endpoint text.
std::string summary_model_identity(const ModelProfile& profile, const AppConfig* config = nullptr);
std::string summary_model_state_path();
void remember_successful_chat_model(const std::string& identity,
                                    const std::string& state_path) noexcept;

class SummaryModelFallback {
public:
    SummaryModelFallback(const AppConfig& config, const std::string& preferred_name,
                         const std::optional<ModelProfile>& contextual = std::nullopt,
                         std::string state_path = summary_model_state_path());

    const std::vector<ModelProfile>& candidates() const { return candidates_; }
    // A task validates its output before accepting a candidate. No callbacks or
    // borrowed configuration survive this synchronous call.
    std::optional<ModelProfile> run(const std::function<bool(const ModelProfile&)>& attempt,
                                    const std::atomic<bool>* cancelled = nullptr) const;

private:
    std::string state_path_;
    std::string preference_key_;
    std::vector<ModelProfile> candidates_;
};

} // namespace acecode
