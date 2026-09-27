#pragma once

#include "web/handlers/models_handler.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <thread>

namespace acecode::web {

struct ModelReasoningSyncBatch {
    ModelProbeRequest connection;
    std::vector<ModelProfile> profiles;
};

std::vector<ModelReasoningSyncBatch> group_model_reasoning_sync(
    const std::vector<ModelProfile>& profiles,
    const std::set<std::string>& names = {});

// Merge only valid declarations into unchanged profiles. Missing declarations
// and profiles edited/deleted since discovery began are successful no-ops.
bool merge_model_reasoning_sync(
    std::vector<ModelProfile>& current,
    const std::vector<ModelProfile>& snapshot,
    const ParsedOpenAiModels& discovered);

class SavedModelReasoningSync {
public:
    using Snapshot = std::function<std::vector<ModelProfile>()>;
    using Probe = std::function<OpenAiModelsProbeResult(
        const ModelProbeRequest&, const std::atomic<bool>&)>;
    using Apply = std::function<void(
        const ModelReasoningSyncBatch&, const ParsedOpenAiModels&)>;

    SavedModelReasoningSync(Snapshot snapshot, Apply apply, Probe probe = {});
    ~SavedModelReasoningSync();
    SavedModelReasoningSync(const SavedModelReasoningSync&) = delete;
    SavedModelReasoningSync& operator=(const SavedModelReasoningSync&) = delete;

    // Empty name requests all saved models. Repeated pending requests coalesce.
    void request(const std::string& name = {});
    void stop();

private:
    void run();
    Snapshot snapshot_;
    Apply apply_;
    Probe probe_;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::mutex stop_mutex_;
    std::condition_variable ready_;
    bool all_pending_ = false;
    std::set<std::string> names_pending_;
    std::thread worker_;
};

} // namespace acecode::web
