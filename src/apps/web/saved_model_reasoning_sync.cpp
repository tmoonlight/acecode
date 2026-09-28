#include "saved_model_reasoning_sync.hpp"

#include <algorithm>
#include <utility>

namespace acecode::web {

std::vector<ModelReasoningSyncBatch> group_model_reasoning_sync(
    const std::vector<ModelProfile>& profiles,
    const std::set<std::string>& names) {
    std::vector<ModelReasoningSyncBatch> batches;
    std::map<std::string, std::size_t> connections;
    for (const auto& profile : profiles) {
        if (profile.provider != "openai" || profile.base_url.empty() ||
            profile.model.empty() || profile.endpoint_mode == "full_url" ||
            (!names.empty() && !names.count(profile.name))) continue;
        ModelProbeRequest request;
        request.catalog_provider_id =
            profile.models_dev_provider_id.value_or("custom-openai");
        request.provider = profile.provider;
        request.base_url = profile.base_url;
        request.api_key = profile.api_key;
        request.request_headers = profile.request_headers;
        const auto key = model_probe_connection_fingerprint(request);
        const auto [it, inserted] = connections.emplace(key, batches.size());
        if (inserted) batches.push_back({std::move(request), {}});
        batches[it->second].profiles.push_back(profile);
    }
    return batches;
}

bool merge_model_reasoning_sync(
    std::vector<ModelProfile>& current,
    const std::vector<ModelProfile>& snapshot,
    const ParsedOpenAiModels& discovered) {
    bool changed = false;
    for (const auto& before : snapshot) {
        const auto declaration = discovered.reasoning.find(before.model);
        if (declaration == discovered.reasoning.end() || !declaration->second ||
            declaration->second->supported_efforts.empty()) continue;
        std::string error;
        auto reasoning = parse_model_reasoning_options(
            model_reasoning_options_to_json(*declaration->second), error);
        if (!reasoning || !reasoning->supported) continue;
        auto found = std::find_if(current.begin(), current.end(),
            [&](const ModelProfile& profile) { return profile.name == before.name; });
        if (found == current.end() || !model_profiles_equal(*found, before)) continue;

        if (found->reasoning) {
            // Discovery owns efforts/defaults; explicit user choices and token
            // budget capability remain attached to the saved profile.
            const auto& previous = *found->reasoning;
            reasoning->mandatory = previous.mandatory || reasoning->mandatory;
            if (reasoning->mandatory) {
                reasoning->default_enabled = true;
                reasoning->enabled.reset();
            } else {
                reasoning->enabled = previous.enabled;
            }
            if (previous.effort &&
                std::find(reasoning->supported_efforts.begin(),
                          reasoning->supported_efforts.end(), *previous.effort) !=
                    reasoning->supported_efforts.end()) {
                reasoning->effort = previous.effort;
            }
            reasoning->supports_max_tokens = previous.supports_max_tokens;
            reasoning->max_tokens = previous.max_tokens;
        }
        ModelProfile next = *found;
        next.reasoning = std::move(reasoning);
        if (std::find(next.capabilities.begin(), next.capabilities.end(),
                      "reasoning") == next.capabilities.end()) {
            next.capabilities.push_back("reasoning");
        }
        if (!model_profiles_equal(*found, next)) {
            *found = std::move(next);
            changed = true;
        }
    }
    return changed;
}

SavedModelReasoningSync::SavedModelReasoningSync(
    Snapshot snapshot, Apply apply, Probe probe)
    : snapshot_(std::move(snapshot)), apply_(std::move(apply)),
      probe_(probe ? std::move(probe) : Probe{
          [](const ModelProbeRequest& request, const std::atomic<bool>& cancel) {
              return probe_openai_models(request, &cancel);
          }}),
      worker_([this] { run(); }) {}

SavedModelReasoningSync::~SavedModelReasoningSync() {
    stop();
}

void SavedModelReasoningSync::request(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_.load()) return;
        if (name.empty()) {
            all_pending_ = true;
            names_pending_.clear();
        } else if (!all_pending_) {
            names_pending_.insert(name);
        }
    }
    ready_.notify_one();
}

void SavedModelReasoningSync::stop() {
    std::lock_guard<std::mutex> stop_lock(stop_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_.store(true);
    }
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void SavedModelReasoningSync::run() {
    for (;;) {
        std::set<std::string> names;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] {
                return stopping_.load() || all_pending_ || !names_pending_.empty();
            });
            if (stopping_.load()) return;
            if (!all_pending_) names.swap(names_pending_);
            all_pending_ = false;
            names_pending_.clear();
        }
        try {
            const auto batches = group_model_reasoning_sync(snapshot_(), names);
            for (const auto& batch : batches) {
                if (stopping_.load()) return;
                try {
                    auto result = probe_(batch.connection, stopping_);
                    if (stopping_.load()) return;
                    if (result.models) apply_(batch, *result.models);
                } catch (...) {
                    // A failed connection must not prevent the other batches.
                }
            }
        } catch (...) {
            // Background refresh is best-effort and never produces UI errors.
        }
    }
}

} // namespace acecode::web
