#include "im/line/line_media.hpp"

#include "im/line/line_protocol.hpp"
#include "platform/crypto/secure_random.hpp"

#include <cstring>

namespace acecode::im::line {

MediaRegistry::MediaRegistry(std::chrono::milliseconds ttl, std::size_t max_entries)
    : ttl_(ttl), max_entries_(max_entries == 0 ? 1 : max_entries) {}

void MediaRegistry::prune_locked(Clock::time_point now) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.expires_at <= now) it = entries_.erase(it);
        else ++it;
    }
    while (entries_.size() >= max_entries_) {
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (it->second.expires_at < oldest->second.expires_at) oldest = it;
        entries_.erase(oldest);
    }
}

std::string MediaRegistry::add(const std::filesystem::path& path, const std::string& name,
                               const std::string& mime_type, Clock::time_point now) {
    const auto token = platform::secure_random_token(32);
    if (token.size() != 32) return {};
    Entry entry;
    entry.path = path;
    entry.name = safe_file_name(name.empty() ? std::string("image") : name);
    entry.mime_type = mime_type;
    entry.expires_at = now + ttl_;
    std::lock_guard<std::mutex> lock(mu_);
    prune_locked(now);
    const auto relative = token + "/" + entry.name;
    entries_[token] = std::move(entry);
    return relative;
}

std::optional<MediaRegistry::Entry> MediaRegistry::find(const std::string& token, const std::string& name,
                                                        Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = entries_.find(token);
    if (it == entries_.end()) return std::nullopt;
    if (it->second.expires_at <= now) {
        entries_.erase(it);
        return std::nullopt;
    }
    if (it->second.name != name) return std::nullopt;
    return it->second;
}

std::optional<MediaRegistry::Entry> MediaRegistry::find_path(const std::string& url_path, Clock::time_point now) {
    const std::size_t prefix = std::strlen(kMediaRoutePrefix);
    if (url_path.compare(0, prefix, kMediaRoutePrefix) != 0) return std::nullopt;
    const auto rest = url_path.substr(prefix);
    const auto slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size()) return std::nullopt;
    const auto token = rest.substr(0, slash);
    const auto name = rest.substr(slash + 1);
    if (name.find('/') != std::string::npos) return std::nullopt;
    return find(token, name, now);
}

std::size_t MediaRegistry::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.size();
}

} // namespace acecode::im::line
