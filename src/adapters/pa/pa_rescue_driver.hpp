#pragma once

#include "pa_overflow_rescue.hpp"
#include "llm/llm_provider.hpp"
#include "session/thread_repair.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace acecode::pa {

// Synchronous, non-owning port. The driver retains no host or service pointer.
// Policy/state stay in PA; the host controls all IO, waiting and publication.
class PaRescueHost {
public:
    virtual ~PaRescueHost() = default;
    virtual int history_tokens() const = 0;
    virtual void note_rejection(int request_tokens) = 0;
    virtual ThreadRepairResult repair(const ThreadRepairOptions& options) = 0;
    // Replace inline images in the history with file references and publish
    // the notice. Returns how many images were withheld (0 = nothing changed).
    virtual int withhold_inline_images() = 0;
    virtual bool wait(int milliseconds) = 0;
    virtual void reset_stream() = 0;
    virtual void history_repaired() = 0;
    virtual void notice(const std::string& text, nlohmann::json metadata) = 0;
    virtual void progress(nlohmann::json payload) = 0;
    virtual void retry(const ProviderErrorInfo& info, bool waiting,
                       std::string label, std::string detail) = 0;
};

// true means re-send, false means cancelled or exhausted. The caller preserves
// the distinction by inspecting its cancellation signal before reporting error.
// request_inline_images: the rejected request carried images as inline image
// data (see RescueInputs::inline_images).
bool run_rescue(PaRescueHost& host, RescueState& state, const ProviderErrorInfo& error,
                int request_tokens, bool& emergency_profile,
                bool request_inline_images = false);
} // namespace acecode::pa
