#include "pa_context_budget.hpp"

#include <algorithm>

namespace acecode::pa {
namespace {

// provider 与 model 拼成 map key。用 '\x1f'(unit separator)分隔,避免
// provider 名里出现分隔符时把两个不同模型撞成同一条观测。
std::string make_key(const std::string& provider, const std::string& model) {
    std::string key;
    key.reserve(provider.size() + model.size() + 1);
    key.append(provider);
    key.push_back('\x1f');
    key.append(model);
    return key;
}

void split_key(const std::string& key, std::string& provider, std::string& model) {
    const std::size_t pos = key.find('\x1f');
    if (pos == std::string::npos) {
        provider = key;
        model.clear();
        return;
    }
    provider = key.substr(0, pos);
    model = key.substr(pos + 1);
}

} // namespace

bool identity_is_known(const std::string& provider, const std::string& model) {
    (void)provider;
    return !model.empty();
}

bool observation_is_credible(int rejected_tokens) {
    return rejected_tokens >= PA_CONTEXT_BUDGET_MIN_CREDIBLE_REJECTION_TOKENS;
}

int suggest_effective_window(int declared_window, int lowest_rejected_tokens) {
    // 没撞过墙、或撞的那次规模小到不可信,都完全不干预 —— 适配层不该在没有
    // 可靠证据的时候猜服务端的限制。
    if (!observation_is_credible(lowest_rejected_tokens)) return declared_window;

    const long long safe =
        static_cast<long long>(lowest_rejected_tokens) *
        PA_CONTEXT_BUDGET_SAFETY_PERCENT / 100;
    const int suggested = static_cast<int>(safe);

    // 声明窗口未知(<= 0)时,观测值就是唯一信息源,直接采用。
    if (declared_window <= 0) return suggested;
    // 只收不放:观测比声明值还宽的时候,说明这次拒绝跟窗口大小无关,保持声明值。
    return std::min(declared_window, suggested);
}

void ContextBudgetLearner::note_rejected(const std::string& provider,
                                         const std::string& model,
                                         int request_tokens) {
    if (!observation_is_credible(request_tokens)) return;
    if (!enabled() || !identity_is_known(provider, model)) return;
    std::lock_guard<std::mutex> lk(mu_);
    Entry& entry = entries_[make_key(provider, model)];
    entry.rejections += 1;
    entry.lowest_rejected_tokens =
        entry.lowest_rejected_tokens == 0
            ? request_tokens
            : std::min(entry.lowest_rejected_tokens, request_tokens);
    // 比新的被拒线还高的「成功」观测已经过时:那次能过是历史,现在过不去了。
    // 留着它只会让诊断输出自相矛盾(显示成功规模大于被拒规模)。
    if (entry.highest_accepted_tokens >= entry.lowest_rejected_tokens) {
        entry.highest_accepted_tokens = 0;
    }
}

void ContextBudgetLearner::note_accepted(const std::string& provider,
                                         const std::string& model,
                                         int request_tokens) {
    if (request_tokens <= 0) return;
    if (!enabled() || !identity_is_known(provider, model)) return;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(make_key(provider, model));
    // 没撞过墙就不建表:绝大多数模型永远走不到这条路径,不该为它们占内存。
    if (it == entries_.end()) return;
    it->second.highest_accepted_tokens =
        std::max(it->second.highest_accepted_tokens, request_tokens);
}

int ContextBudgetLearner::effective_window(const std::string& provider,
                                           const std::string& model,
                                           int declared_window) const {
    if (!enabled() || !identity_is_known(provider, model)) return declared_window;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(make_key(provider, model));
    if (it == entries_.end()) return declared_window;
    return suggest_effective_window(declared_window,
                                    it->second.lowest_rejected_tokens);
}

bool ContextBudgetLearner::has_observation(const std::string& provider,
                                           const std::string& model) const {
    if (!enabled() || !identity_is_known(provider, model)) return false;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(make_key(provider, model));
    return it != entries_.end() && it->second.lowest_rejected_tokens > 0;
}

std::vector<ContextBudgetSnapshot> ContextBudgetLearner::snapshot() const {
    std::vector<ContextBudgetSnapshot> out;
    std::lock_guard<std::mutex> lk(mu_);
    out.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
        ContextBudgetSnapshot item;
        split_key(key, item.provider, item.model);
        item.lowest_rejected_tokens = entry.lowest_rejected_tokens;
        item.highest_accepted_tokens = entry.highest_accepted_tokens;
        item.rejections = entry.rejections;
        out.push_back(std::move(item));
    }
    return out;
}

void ContextBudgetLearner::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    entries_.clear();
}

ContextBudgetLearner& context_budget() {
    static ContextBudgetLearner learner;
    return learner;
}

} // namespace acecode::pa
