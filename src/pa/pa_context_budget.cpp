#include "pa_context_budget.hpp"

#include "../utils/logger.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

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

// 用 steady_clock:回升是按「过了多久」算的,不能被系统时间调整或时区变更
// 拨来拨去。
std::int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
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

int suggest_effective_window(int declared_window,
                             int lowest_rejected_tokens,
                             std::int64_t ms_since_rejection) {
    // 没撞过墙、或撞的那次规模小到不可信,都完全不干预 —— 适配层不该在没有
    // 可靠证据的时候猜服务端的限制。
    if (!observation_is_credible(lowest_rejected_tokens)) return declared_window;

    // AIMD 的「加性增」半边:每隔一个间隔没再被拒,就把被拒线抬一档,直到
    // 封顶于声明窗口。撞墙则由 note_rejected 重置计时起点,回到乘性减。
    long long relaxed = lowest_rejected_tokens;
    if (ms_since_rejection > 0) {
        constexpr long long kIntervalMs =
            static_cast<long long>(PA_CONTEXT_BUDGET_RELAX_INTERVAL_MINUTES) *
            60 * 1000;
        // 档数封顶:每档 ×1.2,不到 70 档就能从下限涨到 int 上限,而进程连着
        // 跑几天算出来的 steps 会大得离谱。多算的部分对结果没有任何影响。
        const long long steps =
            std::min<long long>(ms_since_rejection / kIntervalMs, 100);
        const long long ceiling = declared_window > 0
            ? static_cast<long long>(declared_window)
            : static_cast<long long>(std::numeric_limits<int>::max());
        for (long long i = 0; i < steps && relaxed < ceiling; ++i) {
            relaxed = relaxed * PA_CONTEXT_BUDGET_RELAX_PERCENT / 100;
        }
    }

    // 回升到声明窗口及以上 = 那条观测已经彻底过期,适配层就该完全退出,连
    // 安全余量也不再扣。否则 85% 的折扣会永久留在那里 —— 明明已经一小时没
    // 撞过墙了,却还在按一条早就不成立的旧证据克扣上下文。
    if (declared_window > 0 && relaxed >= declared_window) return declared_window;

    const long long safe = relaxed * PA_CONTEXT_BUDGET_SAFETY_PERCENT / 100;
    const int suggested = static_cast<int>(
        std::min<long long>(safe, std::numeric_limits<int>::max()));

    // 声明窗口未知(<= 0)时,观测值就是唯一信息源,直接采用。
    if (declared_window <= 0) return suggested;
    // 不越过声明值:回升的终点是用户配置的窗口,不是无限往上试。
    return std::min(declared_window, suggested);
}

void ContextBudgetLearner::note_rejected(const std::string& provider,
                                         const std::string& model,
                                         int request_tokens,
                                         std::int64_t now_ms) {
    if (!observation_is_credible(request_tokens)) return;
    if (!enabled() || !identity_is_known(provider, model)) return;
    std::lock_guard<std::mutex> lk(mu_);
    Entry& entry = entries_[make_key(provider, model)];
    entry.rejections += 1;
    // 重置回升计时:这次拒绝证实了限制仍在,之前攒的档数一律作废。
    entry.last_rejection_ms = now_ms > 0 ? now_ms : steady_now_ms();
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
                                           int declared_window,
                                           std::int64_t now_ms) const {
    if (!enabled() || !identity_is_known(provider, model)) return declared_window;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(make_key(provider, model));
    if (it == entries_.end()) return declared_window;

    Entry& entry = it->second;
    const std::int64_t now = now_ms > 0 ? now_ms : steady_now_ms();
    const std::int64_t elapsed = entry.last_rejection_ms > 0
        ? (std::max<std::int64_t>)(0, now - entry.last_rejection_ms)
        : 0;
    const int window = suggest_effective_window(
        declared_window, entry.lowest_rejected_tokens, elapsed);

    // 回升是随时间连续发生的,没有一个可以挂日志的「事件」——只能在算出来的
    // 那一刻比对。变了才写,否则每个回合都刷一行。
    if (window != entry.last_logged_window) {
        const int previous = entry.last_logged_window;
        entry.last_logged_window = window;
        if (previous > 0) {
            LOG_INFO("[pa] compaction window " +
                     std::string(window > previous ? "relaxed" : "tightened") +
                     "; from=" + std::to_string(previous) +
                     " to=" + std::to_string(window) +
                     " declared=" + std::to_string(declared_window) +
                     " lowest_rejected=" +
                     std::to_string(entry.lowest_rejected_tokens) +
                     " minutes_since_rejection=" +
                     std::to_string(elapsed / 60000) +
                     " provider=" + provider + " model=" + model);
        }
    }
    return window;
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
