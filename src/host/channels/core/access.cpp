#include "access.hpp"

#include "channels/core/platforms.hpp"
#include "platform/crypto/secure_random.hpp"

#include <cstdint>

namespace acecode::channels::core {
namespace {

constexpr std::size_t kPinDigits = 6;

// 私聊文字是不是一串 6 位数字(两端空白不算);是就当作绑定码候选。
std::string pin_candidate(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    const auto trimmed = text.substr(begin, end - begin + 1);
    if (trimmed.size() != kPinDigits) return {};
    for (const char c : trimmed)
        if (c < '0' || c > '9') return {};
    return trimmed;
}

bool is_pin(const std::string& code) { return code.size() == kPinDigits && !pin_candidate(code).empty(); }

} // namespace

std::string principal_for(const im::Address& address) {
    const auto* spec = find_platform_spec(address.platform);
    if (address.kind == im::ChatKind::Group && spec && spec->group_scoped_members)
        return "member:" + address.chat + ":" + address.sender;
    return "user:" + address.sender;
}

std::string group_principal(const im::Address& address) { return "group:" + address.chat; }

std::string principal_kind(const std::string& principal) {
    if (principal.rfind("group:", 0) == 0) return "group";
    if (principal.rfind("member:", 0) == 0) return "member";
    return "user";
}

void AccessControl::prune_locked(Clock::time_point now) {
    for (auto it = requests_.begin(); it != requests_.end();) {
        if (it->second.expires <= now) it = requests_.erase(it);
        else ++it;
    }
    for (auto it = owner_codes_.begin(); it != owner_codes_.end();) {
        if (it->second <= now) it = owner_codes_.erase(it);
        else ++it;
    }
}

bool AccessControl::has_pending_locked(const std::string& principal, Clock::time_point now) const {
    for (const auto& [id, request] : requests_)
        if (request.principal == principal && request.expires > now) return true;
    return false;
}

std::optional<PendingRequest> AccessControl::create_locked(const std::string& principal, const im::Inbound& inbound,
                                                           Clock::time_point now) {
    if (has_pending_locked(principal, now) || requests_.size() >= kMaxPending) return std::nullopt;
    PendingRequest request;
    do {
        request.id = platform::secure_random_token(8);
    } while (!request.id.empty() && requests_.count(request.id));
    if (request.id.empty()) return std::nullopt;
    request.principal = principal;
    request.kind = principal_kind(principal);
    request.name = request.kind == "group" ? std::string{} : inbound.sender_name;
    request.address = inbound.address;
    request.expires = now + kRequestTtl;
    requests_[request.id] = request;
    return request;
}

AccessResult AccessControl::evaluate(const PlatformConfig& config, const im::Inbound& inbound,
                                     Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    prune_locked(now);
    AccessResult result;
    result.principal = principal_for(inbound.address);
    result.owner = !config.owner.empty() && config.owner == result.principal;

    if (inbound.address.kind == im::ChatKind::Private) {
        if (config.has_access(result.principal)) {
            result.decision = AccessDecision::Allow;
            result.reason = "authorized";
            return result;
        }
        if (config.owner.empty()) {
            const bool by_link = !inbound.start_code.empty();
            const auto candidate = by_link ? inbound.start_code : pin_candidate(inbound.text);
            const auto code = candidate.empty() ? owner_codes_.end() : owner_codes_.find(candidate);
            if (code != owner_codes_.end()) {
                owner_codes_.erase(code);  // 一次性
                result.claimed_owner = result.owner = true;
                result.decision = AccessDecision::Allow;
                result.consumed = true;
                result.reason = by_link ? "owner link accepted, sender is now the owner"
                                        : "owner code accepted, sender is now the owner";
                return result;
            }
            if (!by_link && !candidate.empty()) {
                bool pins_active = false;
                for (const auto& [value, expires] : owner_codes_) pins_active = pins_active || is_pin(value);
                if (pins_active && ++pin_failures_ >= kMaxPinFailures) {
                    for (auto it = owner_codes_.begin(); it != owner_codes_.end();) {
                        if (is_pin(it->first)) it = owner_codes_.erase(it);
                        else ++it;
                    }
                    pin_failures_ = 0;
                }
            }
            if (owner_window_until_ > now) {
                owner_window_until_ = {};
                result.claimed_owner = result.owner = true;
                result.decision = AccessDecision::Allow;
                result.reason = "first private chat in the owner window, sender is now the owner";
                return result;
            }
        }
        result.created = create_locked(result.principal, inbound, now);
        result.decision = result.created ? AccessDecision::NotifyPending : AccessDecision::Ignore;
        result.reason = result.created ? "not authorized, approval requested"
                                       : "not authorized, a request is already pending";
        return result;
    }

    // 群聊:未点名一律忽略;群本身要先批准;然后发言人要被授权。
    if (!inbound.mentioned) {
        result.reason = "group message without @mention, ignored";
        return result;
    }
    const auto group = group_principal(inbound.address);
    if (!config.has_access(group)) {
        result.created = create_locked(group, inbound, now);  // 不在群里回复,只在设置页出现请求
        result.reason = result.created ? "group not approved, approval requested"
                                       : "group not approved, a request is already pending";
        return result;
    }
    if (config.has_access(result.principal)) {
        result.decision = AccessDecision::Allow;
        result.reason = "authorized";
        return result;
    }
    result.created = create_locked(result.principal, inbound, now);
    result.decision = result.created ? AccessDecision::NotifyPending : AccessDecision::Ignore;
    result.reason = result.created ? "group member not authorized, approval requested"
                                   : "group member not authorized, a request is already pending";
    return result;
}

std::vector<PendingRequest> AccessControl::pending(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    prune_locked(now);
    std::vector<PendingRequest> out;
    for (const auto& [id, request] : requests_) out.push_back(request);
    return out;
}

std::optional<PendingRequest> AccessControl::take(const std::string& id, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    prune_locked(now);
    const auto it = requests_.find(id);
    if (it == requests_.end()) return std::nullopt;
    auto request = it->second;
    requests_.erase(it);
    return request;
}

void AccessControl::drop_principal(const std::string& principal) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = requests_.begin(); it != requests_.end();) {
        if (it->second.principal == principal) it = requests_.erase(it);
        else ++it;
    }
}

std::string AccessControl::issue_owner_pin(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    prune_locked(now);
    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto random = platform::secure_random_bytes(4);
        if (random.size() != 4) return {};
        std::uint32_t value = 0;
        for (const unsigned char byte : random) value = (value << 8) | byte;
        std::string code = std::to_string(value % 1000000u);
        code.insert(0, kPinDigits - code.size(), '0');
        if (owner_codes_.count(code)) continue;
        owner_codes_[code] = now + kOwnerTtl;
        pin_failures_ = 0;
        return code;
    }
    return {};
}

std::string AccessControl::issue_owner_code(Clock::time_point now) {
    const auto code = platform::secure_random_token(24);
    if (code.empty()) return {};
    std::lock_guard<std::mutex> lock(mu_);
    prune_locked(now);
    owner_codes_[code] = now + kOwnerTtl;
    return code;
}

void AccessControl::open_owner_window(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    owner_window_until_ = now + kOwnerTtl;
}

bool AccessControl::owner_window_open(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mu_);
    return owner_window_until_ > now;
}

} // namespace acecode::channels::core
